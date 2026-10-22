//===- MipsR5900VU0MatVec.cpp - R5900 VU0 matrix times vector -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A transform written with scalar structs computes each row as a sum of
// products of loop-invariant matrix entries with the lanes of one vector in
// memory:
//
//   x = m[0].x * v.x + m[0].y * v.y + m[0].z * v.z + m[0].w
//   y = m[1].x * v.x + m[1].y * v.y + m[1].z * v.z + m[1].w
//   ...
//
// Neither vectorizer makes good VU0 code of this: the loop vectorizer
// transposes 4 iterations through the GPRs, and SLP costs the matrix columns
// as if they were gathered in the loop. This pass, which runs before them,
// builds the columns (and the translation) before the loop and rewrites the
// rows as one vector load and
//
//   col_x * v.xxxx + col_y * v.yyyy + col_z * v.zzzz + t
//
// which the VU0 broadcast multiply combine and the ACC chain pass make
// VMULAx, VMADDAy and VMADDz. Each row becomes an extract of the result.
//
//===----------------------------------------------------------------------===//

#include "MipsR5900VU0MatVec.h"
#include "MipsSubtarget.h"
#include "MipsTargetMachine.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/Loads.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/SimplifyQuery.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

#define DEBUG_TYPE "mips-r5900-vu0-matvec"

STATISTIC(NumMatVec, "Number of VU0 matrix times vector rewrites");

static cl::opt<bool> EnableMatVec(
    "mips-r5900-vu0-matvec", cl::init(true), cl::Hidden,
    cl::desc("Rewrite rows of dot products with one vector as a VU0 matrix "
             "times vector"));

namespace {

/// A row: the sum of Coef[J] * v[J] plus Trans, any of which may be absent.
/// Loads[J] is the load of v[J].
struct Row {
  Instruction *Root = nullptr;
  Value *Coef[4] = {};
  LoadInst *Loads[4] = {};
  Value *Trans = nullptr;
  unsigned NumTerms = 0;
  FastMathFlags FMF = FastMathFlags::getFast();
};

class MatVec {
  const DataLayout &DL;
  Loop *L;
  BasicBlock *BB;
  ScalarEvolution &SE;
  DominatorTree &DT;
  AssumptionCache &AC;

  /// The R5900 FPU does the scalar operations in hardware only without NaNs
  /// and infinities, and the sums are reassociated.
  static bool hasFlags(Row &R, const Instruction *I) {
    FastMathFlags F = I->getFastMathFlags();
    R.FMF &= F;
    return F.allowReassoc() && F.noNaNs() && F.noInfs();
  }

  /// Add Coef * V to R, where V is a float load at lane offset from Base.
  bool addTerm(Row &R, Value *Coef, Value *V, Value *&Base) {
    auto *LI = dyn_cast<LoadInst>(V);
    if (!LI || !LI->isSimple() || LI->getParent() != BB ||
        !LI->getType()->isFloatTy() || !L->isLoopInvariant(Coef))
      return false;
    int64_t Offset;
    Value *B =
        GetPointerBaseWithConstantOffset(LI->getPointerOperand(), Offset, DL);
    if (Offset < 0 || Offset > 12 || Offset % 4 || (Base && B != Base))
      return false;
    Base = B;
    unsigned J = Offset / 4;
    if (R.Coef[J])
      return false;
    R.Coef[J] = Coef;
    R.Loads[J] = LI;
    ++R.NumTerms;
    return true;
  }

  bool collect(Row &R, Value *V, Value *&Base, unsigned Depth) {
    if (Depth > 8)
      return false;
    auto *I = dyn_cast<Instruction>(V);
    if (I && I->getParent() == BB && I->getOpcode() == Instruction::FAdd) {
      if ((I != R.Root && !I->hasOneUse()) || !hasFlags(R, I))
        return false;
      return collect(R, I->getOperand(0), Base, Depth + 1) &&
             collect(R, I->getOperand(1), Base, Depth + 1);
    }
    if (I && I->getParent() == BB && I->getOpcode() == Instruction::FMul) {
      if (!I->hasOneUse() || !hasFlags(R, I))
        return false;
      Value *A = I->getOperand(0), *B = I->getOperand(1);
      if (L->isLoopInvariant(B))
        std::swap(A, B);
      return addTerm(R, A, B, Base);
    }
    if (L->isLoopInvariant(V)) {
      if (R.Trans)
        return false;
      R.Trans = V;
      ++R.NumTerms;
      return true;
    }
    return addTerm(R, ConstantFP::get(V->getType(), 1.0), V, Base);
  }

  static Value *buildVector(IRBuilderBase &B, ArrayRef<Value *> Lanes,
                            Type *Ty) {
    Value *Vec = Constant::getNullValue(FixedVectorType::get(Ty, 4));
    for (auto [I, V] : enumerate(Lanes))
      if (V)
        Vec = B.CreateInsertElement(Vec, V, I);
    return Vec;
  }

  Value *addLanewiseOps(IRBuilderBase &B, IRBuilderBase &PB, Value *Acc,
                        MutableArrayRef<Instruction *> Ends);
  bool rewrite(Value *Base, MutableArrayRef<Row> Rows);

public:
  MatVec(const DataLayout &DL, Loop *L, BasicBlock *BB, ScalarEvolution &SE,
         DominatorTree &DT, AssumptionCache &AC)
      : DL(DL), L(L), BB(BB), SE(SE), DT(DT), AC(AC) {}
  bool run();
};

} // namespace

/// Continue Acc with the operations that every row goes through in the same
/// way, such as a scale by a loop-invariant value or a conversion to int, as
/// VU0 lane-wise operations. Ends are the rows and become the scalar results
/// these replace.
Value *MatVec::addLanewiseOps(IRBuilderBase &B, IRBuilderBase &PB, Value *Acc,
                              MutableArrayRef<Instruction *> Ends) {
  while (true) {
    SmallVector<Value *, 4> Others;
    Instruction *U0 = nullptr;
    unsigned OpIdx = 0;
    for (Instruction *E : Ends) {
      if (!E->hasOneUse())
        return Acc;
      auto *U = cast<Instruction>(*E->user_begin());
      if (U->getParent() != BB || (U0 && (U->getOpcode() != U0->getOpcode() ||
                                          U->getType() != U0->getType())))
        return Acc;
      if (!U0)
        U0 = U;
      if (U->getOpcode() == Instruction::FPToSI)
        continue;
      if (U->getOpcode() != Instruction::FMul &&
          U->getOpcode() != Instruction::FAdd &&
          U->getOpcode() != Instruction::FSub)
        return Acc;
      // The scalar operation with a loop-invariant value, in the same
      // operand for all rows, and in hardware on the R5900 FPU.
      unsigned Idx = U->getOperand(0) == E ? 1 : 0;
      FastMathFlags F = U->getFastMathFlags();
      if ((U != U0 && Idx != OpIdx) ||
          !L->isLoopInvariant(U->getOperand(Idx)) || !F.noNaNs() || !F.noInfs())
        return Acc;
      OpIdx = Idx;
      Others.push_back(U->getOperand(Idx));
    }

    if (U0->getOpcode() == Instruction::FPToSI) {
      Acc = B.CreateFPToSI(Acc, FixedVectorType::get(U0->getType(), 4));
    } else {
      Value *K = buildVector(PB, Others, U0->getType());
      FastMathFlags FMF = U0->getFastMathFlags();
      for (Instruction *E : Ends)
        FMF &= cast<Instruction>(*E->user_begin())->getFastMathFlags();
      Value *LHS = OpIdx ? Acc : K, *RHS = OpIdx ? K : Acc;
      Acc = B.CreateBinOpFMF(Instruction::BinaryOps(U0->getOpcode()), LHS, RHS,
                             FMF);
    }
    for (Instruction *&E : Ends)
      E = cast<Instruction>(*E->user_begin());
  }
}

bool MatVec::rewrite(Value *Base, MutableArrayRef<Row> Rows) {
  // The lanes the rows use, the loads of them and the range they span.
  bool Used[4] = {}, Loaded[4] = {};
  bool HasTrans = false;
  unsigned NumTerms = 0;
  LoadInst *First = nullptr, *Last = nullptr;
  FastMathFlags FMF = FastMathFlags::getFast();
  for (Row &R : Rows) {
    for (unsigned J = 0; J < 4; ++J) {
      Used[J] |= R.Coef[J] != nullptr;
      LoadInst *LI = R.Loads[J];
      if (!LI)
        continue;
      Loaded[J] = true;
      if (!First || LI->comesBefore(First))
        First = LI;
      if (!Last || Last->comesBefore(LI))
        Last = LI;
    }
    HasTrans |= R.Trans != nullptr;
    NumTerms += R.NumTerms;
    FMF &= R.FMF;
  }
  unsigned NumCols = count(Used, true);

  // A scalar term is about one FPU multiply-add; the vector code is a load,
  // a VU0 operation per column and the translation, and an extract per row.
  if (1 + NumCols + HasTrans + Rows.size() >= NumTerms)
    return false;

  // The vector load reads all 16 bytes at the place of the last lane load.
  auto *VecTy = FixedVectorType::get(Type::getFloatTy(BB->getContext()), 4);
  Instruction *InsertPt = Last->getNextNode();
  // The alignment of Base, as known or as the load of lane x assumes.
  Align BaseAlign = getKnownAlignment(Base, DL, InsertPt, &AC, &DT);
  for (Row &R : Rows)
    if (R.Loads[0])
      BaseAlign = std::max(BaseAlign, R.Loads[0]->getAlign());
  if (BaseAlign < Align(16))
    return false;
  // Without a load of every lane, the 16 bytes must be dereferenceable, such
  // as within an array the loop walks.
  if (!all_of(Loaded, [](bool B) { return B; }) &&
      !isDereferenceableAndAlignedPointer(
          Base, VecTy, Align(16), SimplifyQuery(DL, &DT, &AC, InsertPt)) &&
      !isDereferenceableAndAlignedInLoop(
          SE.getSCEV(Base), Align(16),
          SE.getConstant(DL.getIndexType(Base->getType()), 16), L, SE, DT, &AC))
    return false;
  for (Instruction *I = First; I != Last; I = I->getNextNode())
    if (I->mayWriteToMemory())
      return false;
  for (Row &R : Rows)
    if (R.Root->comesBefore(InsertPt))
      return false;

  LLVM_DEBUG(dbgs() << "R5900 VU0 MatVec: " << Rows.size() << " rows, "
                    << NumCols << " columns in " << BB->getName() << "\n");

  // The columns and the translation, before the loop.
  IRBuilder<> PB(L->getLoopPreheader()->getTerminator());
  Value *Cols[4] = {};
  for (unsigned J = 0; J < 4; ++J) {
    if (!Used[J])
      continue;
    SmallVector<Value *, 4> Lanes(4);
    for (auto [I, R] : enumerate(Rows))
      Lanes[I] = R.Coef[J];
    Cols[J] = buildVector(PB, Lanes, PB.getFloatTy());
  }
  Value *Trans = nullptr;
  if (HasTrans) {
    SmallVector<Value *, 4> Lanes(4);
    for (auto [I, R] : enumerate(Rows))
      Lanes[I] = R.Trans;
    Trans = buildVector(PB, Lanes, PB.getFloatTy());
  }

  // The rows, after the last lane load.
  IRBuilder<> B(InsertPt);
  B.setFastMathFlags(FMF);
  Value *Vec = B.CreateAlignedLoad(VecTy, Base, Align(16), "matvec.v");
  Value *Acc = nullptr;
  for (unsigned J = 0; J < 4; ++J) {
    if (!Cols[J])
      continue;
    int Lane = J;
    Value *Prod = B.CreateFMul(
        Cols[J], B.CreateShuffleVector(Vec, {Lane, Lane, Lane, Lane}));
    Acc = Acc ? B.CreateFAdd(Acc, Prod) : Prod;
  }
  if (Trans)
    Acc = B.CreateFAdd(Acc, Trans);
  SmallVector<Instruction *, 4> Ends;
  for (Row &R : Rows)
    Ends.push_back(R.Root);
  Acc = addLanewiseOps(B, PB, Acc, Ends);

  // Extract all rows before deleting the scalar code, which includes the
  // insertion point.
  SmallVector<Value *, 4> Results;
  for (unsigned I = 0; I < Ends.size(); ++I)
    Results.push_back(B.CreateExtractElement(Acc, I));
  for (auto [E, Res] : zip(Ends, Results)) {
    E->replaceAllUsesWith(Res);
    RecursivelyDeleteTriviallyDeadInstructions(E);
  }
  ++NumMatVec;
  return true;
}

bool MatVec::run() {
  // The rows, by the base of their vector, in program order.
  MapVector<Value *, SmallVector<Row, 4>> Groups;
  for (Instruction &I : *BB) {
    if (I.getOpcode() != Instruction::FAdd || !I.getType()->isFloatTy())
      continue;
    // Not part of a larger sum.
    if (I.hasOneUse()) {
      auto *U = cast<Instruction>(*I.user_begin());
      if (U->getParent() == BB && U->getOpcode() == Instruction::FAdd)
        continue;
    }
    Row R;
    R.Root = &I;
    Value *Base = nullptr;
    if (!collect(R, &I, Base, 0) || !Base || R.NumTerms < 2)
      continue;
    auto &Rows = Groups[Base];
    if (Rows.size() < 4)
      Rows.push_back(R);
  }

  bool Changed = false;
  for (auto &[Base, Rows] : Groups)
    if (Rows.size() >= 2)
      Changed |= rewrite(Base, Rows);
  return Changed;
}

PreservedAnalyses MipsR5900VU0MatVecPass::run(Function &F,
                                              FunctionAnalysisManager &AM) {
  const MipsSubtarget *ST = TM.getSubtargetImpl(F);
  if (!EnableMatVec || !ST->isR5900() || !ST->hasVU0())
    return PreservedAnalyses::all();

  auto &LI = AM.getResult<LoopAnalysis>(F);
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &AC = AM.getResult<AssumptionAnalysis>(F);
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
  bool Changed = false;
  for (Loop *L : LI.getLoopsInPreorder()) {
    if (!L->getLoopPreheader())
      continue;
    for (BasicBlock *BB : L->blocks())
      if (LI.getLoopFor(BB) == L)
        Changed |= MatVec(F.getDataLayout(), L, BB, SE, DT, AC).run();
  }
  if (!Changed)
    return PreservedAnalyses::all();
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
