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
// Outside a loop, the same rows with the entries of a 16-byte aligned 4x4
// matrix m[i][j] in memory, as in a matrix times vector function, take the
// columns from the four rows loaded as vectors and transposed (MMI PEXTLW,
// PEXTUW, PCPYLD and PCPYUD).
//
// Before that, a sum that a block adds to only for a positive value d, as
// diffuse lighting does for each light,
//
//   if (d > 0) { r += d * c.x; g += d * c.y; ... }
//
// is the sum with max(d, 0) instead, without the branch: the additions then
// add 0 (fast-math), and the rows are in one block.
//
//===----------------------------------------------------------------------===//

#include "MipsR5900VU0MatVec.h"
#include "MipsSubtarget.h"
#include "MipsTargetMachine.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/DomTreeUpdater.h"
#include "llvm/Analysis/Loads.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/SimplifyQuery.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;
using namespace llvm::PatternMatch;

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
  /// The lanes are those of a <4 x float> value (the Base of the rows), not
  /// loads.
  bool FromVector = false;
};

class MatVec {
  const DataLayout &DL;
  Loop *L;
  BasicBlock *BB;
  ScalarEvolution &SE;
  DominatorTree &DT;
  AssumptionCache &AC;
  AAResults &AA;

  /// The R5900 FPU does the scalar operations in hardware only without NaNs
  /// and infinities, and the sums are reassociated.
  static bool hasFlags(Row &R, const Instruction *I) {
    FastMathFlags F = I->getFastMathFlags();
    R.FMF &= F;
    return F.allowReassoc() && F.noNaNs() && F.noInfs();
  }

  /// Whether V is the same in every row: loop-invariant, or outside a loop
  /// a constant.
  bool isInvariant(const Value *V) const {
    return L ? L->isLoopInvariant(V) : isa<Constant>(V);
  }

  /// Whether \p V is a float load at a constant offset from \p Base.
  bool isLoadFrom(Value *V, Value *Base) const {
    auto *LI = dyn_cast<LoadInst>(V);
    int64_t Offset;
    return LI && LI->getType()->isFloatTy() &&
           GetPointerBaseWithConstantOffset(LI->getPointerOperand(), Offset,
                                            DL) == Base;
  }

  /// Add Coef * V to R, where V is a float load at lane offset from Base.
  /// Outside a loop, Coef is a matrix entry, which rewrite checks.
  bool addTerm(Row &R, Value *Coef, Value *V, Value *&Base) {
    auto *LI = dyn_cast<LoadInst>(V);
    if (!LI || !LI->isSimple() || LI->getParent() != BB ||
        !LI->getType()->isFloatTy() || (L && !L->isLoopInvariant(Coef)) ||
        R.FromVector)
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

  /// Add Coef * Vec[J] to R, a row of the lanes of Vec.
  bool addVectorTerm(Row &R, Value *Coef, Value *Vec, unsigned J,
                     Value *&Base) {
    auto *VTy = dyn_cast<FixedVectorType>(Vec->getType());
    if (!VTy || VTy->getNumElements() != 4 ||
        !VTy->getElementType()->isFloatTy() || J > 3 || R.Coef[J] ||
        (Base && Base != Vec) || (R.NumTerms && !R.FromVector))
      return false;
    Base = Vec;
    R.FromVector = true;
    R.Coef[J] = Coef;
    ++R.NumTerms;
    return true;
  }

  /// \p Coef times \p Scale (a constant, or null for 1), or null if that is
  /// not a constant.
  static Value *scaled(Value *Coef, ConstantFP *Scale) {
    if (!Scale)
      return Coef;
    auto *C = dyn_cast<ConstantFP>(Coef);
    if (!C)
      return nullptr;
    APFloat F = C->getValueAPF();
    F.multiply(Scale->getValueAPF(), APFloat::rmNearestTiesToEven);
    return ConstantFP::get(C->getType(), F);
  }

  /// A lane of a vector, Vec[J] or (Vec * K)[J] with K a constant vector,
  /// times Scale: as a term of R.
  bool collectLane(Row &R, ExtractElementInst *EE, Value *&Base,
                   ConstantFP *Scale) {
    auto *Idx = dyn_cast<ConstantInt>(EE->getIndexOperand());
    if (!L || !Idx)
      return false;
    unsigned J = Idx->getZExtValue();
    Value *Src = EE->getVectorOperand();
    if (auto *M = dyn_cast<Instruction>(Src);
        M && M->getOpcode() == Instruction::FMul && M->getParent() == BB &&
        hasFlags(R, M) && all_of(M->users(), [](const User *U) {
          return isa<ExtractElementInst>(U);
        })) {
      Value *A = M->getOperand(0), *K = M->getOperand(1);
      if (isa<Constant>(A))
        std::swap(A, K);
      if (auto *C = dyn_cast<Constant>(K))
        if (Constant *CJ = C->getAggregateElement(J); CJ && isa<ConstantFP>(CJ))
          if (Value *Coef = scaled(CJ, Scale))
            return addVectorTerm(R, Coef, A, J, Base);
    }
    Value *Coef = scaled(ConstantFP::get(EE->getType(), 1.0), Scale);
    return Coef && addVectorTerm(R, Coef, Src, J, Base);
  }

  /// Add V times Scale (a constant, or null for 1) to R. A product of a
  /// constant and a sum is the sum of the scaled terms; a product may have
  /// other uses, which keep it.
  bool collect(Row &R, Value *V, Value *&Base, unsigned Depth,
               ConstantFP *Scale = nullptr) {
    if (Depth > 8)
      return false;
    auto *I = dyn_cast<Instruction>(V);
    if (auto *EE = dyn_cast_or_null<ExtractElementInst>(I);
        EE && EE->getParent() == BB)
      return collectLane(R, EE, Base, Scale);
    if (I && I->getParent() == BB && I->getOpcode() == Instruction::FAdd) {
      if ((I != R.Root && !I->hasOneUse()) || !hasFlags(R, I))
        return false;
      return collect(R, I->getOperand(0), Base, Depth + 1, Scale) &&
             collect(R, I->getOperand(1), Base, Depth + 1, Scale);
    }
    if (I && I->getParent() == BB && I->getOpcode() == Instruction::FMul) {
      if (!hasFlags(R, I))
        return false;
      Value *A = I->getOperand(0), *B = I->getOperand(1);
      // The vector lane: the operand that varies in the loop, or outside a
      // loop the load from Base (which run picks).
      if (L ? L->isLoopInvariant(B) : isLoadFrom(A, Base))
        std::swap(A, B);
      auto *K = dyn_cast<ConstantFP>(A);
      auto *Sum = dyn_cast<Instruction>(B);
      if (K && Sum && Sum->getParent() == BB &&
          Sum->getOpcode() == Instruction::FAdd && Sum->hasOneUse()) {
        auto *NewScale = cast<ConstantFP>(scaled(K, Scale));
        return hasFlags(R, Sum) &&
               collect(R, Sum->getOperand(0), Base, Depth + 1, NewScale) &&
               collect(R, Sum->getOperand(1), Base, Depth + 1, NewScale);
      }
      Value *Coef = scaled(A, Scale);
      if (!Coef)
        return false;
      if (auto *EE = dyn_cast<ExtractElementInst>(B);
          EE && L && L->isLoopInvariant(Coef))
        if (auto *Idx = dyn_cast<ConstantInt>(EE->getIndexOperand()))
          return addVectorTerm(R, Coef, EE->getVectorOperand(),
                               Idx->getZExtValue(), Base);
      return addTerm(R, Coef, B, Base);
    }
    if (L && L->isLoopInvariant(V)) {
      Value *T = scaled(V, Scale);
      if (R.Trans || !T)
        return false;
      R.Trans = T;
      ++R.NumTerms;
      return true;
    }
    Value *One = scaled(ConstantFP::get(V->getType(), 1.0), Scale);
    return One && addTerm(R, One, V, Base);
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
                        MutableArrayRef<Instruction *> Ends,
                        unsigned *NumSteps = nullptr);
  Value *getMatrix(ArrayRef<Row> Rows) const;
  bool checkLoads(Value *Base, ArrayRef<Row> Rows, ArrayRef<bool> Loaded,
                  LoadInst *First, LoadInst *Last, Value *Matrix,
                  Instruction *&InsertPt);
  bool rewrite(Value *Base, MutableArrayRef<Row> Rows);

public:
  MatVec(const DataLayout &DL, Loop *L, BasicBlock *BB, ScalarEvolution &SE,
         DominatorTree &DT, AssumptionCache &AC, AAResults &AA)
      : DL(DL), L(L), BB(BB), SE(SE), DT(DT), AC(AC), AA(AA) {}
  bool run();
};

} // namespace

/// Whether \p U is an operation that addLanewiseOps continues a row with.
static bool isLanewiseOp(const Instruction *U) {
  if (auto *II = dyn_cast<IntrinsicInst>(U))
    return II->getIntrinsicID() == Intrinsic::maxnum ||
           II->getIntrinsicID() == Intrinsic::minnum;
  return U->getOpcode() == Instruction::FPToSI ||
         U->getOpcode() == Instruction::FPToUI ||
         U->getOpcode() == Instruction::FMul ||
         U->getOpcode() == Instruction::FAdd ||
         U->getOpcode() == Instruction::FSub;
}

/// Whether \p A and \p B are the same operation, up to their operands.
static bool isSameOp(const Instruction *A, const Instruction *B) {
  if (A->getOpcode() != B->getOpcode() || A->getType() != B->getType())
    return false;
  auto *IA = dyn_cast<IntrinsicInst>(A), *IB = dyn_cast<IntrinsicInst>(B);
  return !IA || (IB && IA->getIntrinsicID() == IB->getIntrinsicID());
}

/// Continue Acc with the operations that every row goes through in the same
/// way, such as a scale by a loop-invariant value or a conversion to int, as
/// VU0 lane-wise operations; a row without a multiply that the others have
/// is multiplied by 1. Ends are the rows and become the scalar results these
/// replace. With NumSteps, only counts the operations there. An unsigned
/// conversion is a signed one (VFTOI0) when the operations before bound the
/// value below 2^31, such as min(x, 1) * 128 for a color.
Value *MatVec::addLanewiseOps(IRBuilderBase &B, IRBuilderBase &PB, Value *Acc,
                              MutableArrayRef<Instruction *> Ends,
                              unsigned *NumSteps) {
  // An upper bound of the lanes, from the operations with constants so far.
  std::optional<double> Bound;
  auto MaxOf = [](ArrayRef<Value *> Ks) -> std::optional<double> {
    double M = 0;
    for (Value *K : Ks) {
      auto *C = dyn_cast<ConstantFP>(K);
      if (!C || C->getValueAPF().isNaN() || C->getValueAPF().isInfinity())
        return std::nullopt;
      M = std::max(M, C->getValueAPF().convertToDouble());
    }
    return M;
  };
  auto UserOf = [&](Instruction *E) -> Instruction * {
    if (!E->hasOneUse())
      return nullptr;
    auto *U = cast<Instruction>(*E->user_begin());
    return U->getParent() == BB && isLanewiseOp(U) ? U : nullptr;
  };
  while (true) {
    Instruction *U0 = nullptr;
    for (Instruction *E : Ends)
      if ((U0 = UserOf(E)))
        break;
    if (!U0)
      return Acc;
    SmallVector<Value *, 4> Others;
    SmallVector<bool, 4> Has;
    unsigned OpIdx = ~0u;
    for (Instruction *E : Ends) {
      Instruction *U = UserOf(E);
      if (!U || !isSameOp(U, U0)) {
        if (U0->getOpcode() != Instruction::FMul)
          return Acc;
        Others.push_back(ConstantFP::get(E->getType(), 1.0));
        Has.push_back(false);
        continue;
      }
      Has.push_back(true);
      if (U->getOpcode() == Instruction::FPToSI)
        continue;
      if (U->getOpcode() == Instruction::FPToUI) {
        if (!Bound || *Bound >= 2147483648.0)
          return Acc;
        continue;
      }
      // The scalar operation with a loop-invariant value, in the same
      // operand for all rows, and in hardware on the R5900 FPU.
      unsigned Idx = U->getOperand(0) == E ? 1 : 0;
      FastMathFlags F = U->getFastMathFlags();
      if ((OpIdx != ~0u && Idx != OpIdx) || !isInvariant(U->getOperand(Idx)) ||
          !F.noNaNs() || !F.noInfs())
        return Acc;
      OpIdx = Idx;
      Others.push_back(U->getOperand(Idx));
    }

    // The bound after this operation.
    auto *II0 = dyn_cast<IntrinsicInst>(U0);
    if (II0 && II0->getIntrinsicID() == Intrinsic::minnum) {
      std::optional<double> M = MaxOf(Others);
      Bound = Bound && M ? std::min(*Bound, *M) : (M ? M : Bound);
    } else if (U0->getOpcode() == Instruction::FMul && Bound && *Bound >= 0) {
      // MaxOf: all constants.
      std::optional<double> M = MaxOf(Others);
      bool NonNeg = M && all_of(Others, [](Value *K) {
                      return !cast<ConstantFP>(K)->getValueAPF().isNegative();
                    });
      Bound = NonNeg ? std::optional(*Bound * *M) : std::nullopt;
    } else if (U0->getOpcode() != Instruction::FPToSI &&
               U0->getOpcode() != Instruction::FPToUI) {
      Bound = std::nullopt;
    }

    if (NumSteps) {
      ++*NumSteps;
    } else if (U0->getOpcode() == Instruction::FPToSI ||
               U0->getOpcode() == Instruction::FPToUI) {
      Acc = B.CreateFPToSI(Acc, FixedVectorType::get(U0->getType(), 4));
    } else {
      // The lanes without a row are not used: repeat lane 0 there, so that a
      // common value stays a splat, such as the scale of a fixed-point
      // conversion.
      Others.resize(4, Others[0]);
      Value *K = buildVector(PB, Others, U0->getType());
      FastMathFlags FMF = U0->getFastMathFlags();
      for (auto [E, H] : zip(Ends, Has))
        if (H)
          FMF &= cast<Instruction>(*E->user_begin())->getFastMathFlags();
      Value *LHS = OpIdx ? Acc : K, *RHS = OpIdx ? K : Acc;
      if (auto *II = dyn_cast<IntrinsicInst>(U0))
        Acc = B.CreateBinaryIntrinsic(II->getIntrinsicID(), LHS, RHS, FMF);
      else
        Acc = B.CreateBinOpFMF(Instruction::BinaryOps(U0->getOpcode()), LHS,
                               RHS, FMF);
    }
    for (auto [E, H] : zip(Ends, Has))
      if (H)
        E = cast<Instruction>(*E->user_begin());
  }
}

/// Outside a loop: the 16-byte aligned 4x4 matrix whose entry [I][J] is the
/// coefficient of lane J in row I, or null.
Value *MatVec::getMatrix(ArrayRef<Row> Rows) const {
  if (Rows.size() != 4)
    return nullptr;
  Value *M = nullptr;
  for (auto [I, R] : enumerate(Rows)) {
    if (R.Trans)
      return nullptr;
    for (unsigned J = 0; J < 4; ++J) {
      auto *LI = dyn_cast_or_null<LoadInst>(R.Coef[J]);
      int64_t Offset;
      if (!LI || !LI->isSimple() || LI->getParent() != BB ||
          !LI->getType()->isFloatTy() || !LI->hasOneUse())
        return nullptr;
      Value *B =
          GetPointerBaseWithConstantOffset(LI->getPointerOperand(), Offset, DL);
      if ((M && B != M) || Offset != int64_t(16 * I + 4 * J))
        return nullptr;
      M = B;
    }
  }
  Align MAlign = std::max(getKnownAlignment(M, DL, nullptr, &AC, &DT),
                          cast<LoadInst>(Rows[0].Coef[0])->getAlign());
  return MAlign >= Align(16) ? M : nullptr;
}

/// For rows of loads: the place of the vector load, after the last lane
/// load, if Base is aligned, the 16 bytes may be read and nothing between
/// the lane loads writes memory (outside a loop, the matrix loads checked
/// that).
bool MatVec::checkLoads(Value *Base, ArrayRef<Row> Rows, ArrayRef<bool> Loaded,
                        LoadInst *First, LoadInst *Last, Value *Matrix,
                        Instruction *&InsertPt) {
  auto *VecTy = FixedVectorType::get(Type::getFloatTy(BB->getContext()), 4);
  // The vector load reads all 16 bytes at the place of the last lane load.
  InsertPt = Last->getNextNode();
  // The alignment of Base, as known or as the load of lane x assumes.
  Align BaseAlign = getKnownAlignment(Base, DL, InsertPt, &AC, &DT);
  for (const Row &R : Rows)
    if (R.Loads[0])
      BaseAlign = std::max(BaseAlign, R.Loads[0]->getAlign());
  if (BaseAlign < Align(16))
    return false;
  // Without a load of every lane, the 16 bytes must be dereferenceable, such
  // as within an array the loop walks.
  if (!all_of(Loaded, [](bool B) { return B; }) &&
      !isDereferenceableAndAlignedPointer(
          Base, VecTy, Align(16), SimplifyQuery(DL, &DT, &AC, InsertPt)) &&
      (!L || !isDereferenceableAndAlignedInLoop(
                 SE.getSCEV(Base), Align(16),
                 SE.getConstant(DL.getIndexType(Base->getType()), 16), L, SE,
                 DT, &AC)))
    return false;
  for (Instruction *I = First; I != Last; I = I->getNextNode())
    if (I->mayWriteToMemory() && !Matrix)
      return false;
  for (const Row &R : Rows)
    if (R.Root->comesBefore(InsertPt))
      return false;
  return true;
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
  // Outside a loop the columns are those of a matrix in memory, loaded at
  // the vector: what is written between the loads (such as the results of
  // the rows) must not be any of them.
  Value *Matrix = nullptr;
  if (!L) {
    Matrix = getMatrix(Rows);
    if (!Matrix || NumCols != 4)
      return false;
    SmallVector<LoadInst *, 20> Loads;
    for (Row &R : Rows)
      for (unsigned J = 0; J < 4; ++J) {
        Loads.push_back(cast<LoadInst>(R.Coef[J]));
        if (R.Loads[J])
          Loads.push_back(R.Loads[J]);
      }
    Instruction *Lo = Loads[0], *Hi = Loads[0];
    for (LoadInst *LI : Loads) {
      if (LI->comesBefore(Lo))
        Lo = LI;
      if (Hi->comesBefore(LI))
        Hi = LI;
    }
    for (Instruction *I = Lo; I != Hi; I = I->getNextNode())
      if (I->mayWriteToMemory() && any_of(Loads, [&](LoadInst *LI) {
            return isModSet(AA.getModRefInfo(I, MemoryLocation::get(LI)));
          }))
        return false;
  }

  // A scalar term is about one FPU multiply-add, as is a scalar operation
  // that continues as a lane-wise one; the vector code is a load, a VU0
  // operation per column and the translation, and an extract per row.
  SmallVector<Instruction *, 4> DryEnds;
  for (Row &R : Rows)
    DryEnds.push_back(R.Root);
  unsigned Steps = 0;
  IRBuilder<> DryB(BB->getContext());
  addLanewiseOps(DryB, DryB, nullptr, DryEnds, &Steps);
  if (1 + NumCols + HasTrans + Rows.size() >=
      NumTerms + Steps * (Rows.size() - 1))
    return false;

  auto *VecTy = FixedVectorType::get(Type::getFloatTy(BB->getContext()), 4);
  bool FromVector = Rows[0].FromVector;
  Instruction *InsertPt = nullptr;
  if (FromVector) {
    // The lanes of a vector: before the first row.
    InsertPt = Rows[0].Root;
    for (Row &R : Rows)
      if (R.Root->comesBefore(InsertPt))
        InsertPt = R.Root;
  } else if (!checkLoads(Base, Rows, Loaded, First, Last, Matrix, InsertPt)) {
    return false;
  }

  LLVM_DEBUG(dbgs() << "R5900 VU0 MatVec: " << Rows.size() << " rows, "
                    << NumCols << " columns in " << BB->getName() << "\n");

  // The columns and the translation, before the loop (outside a loop, at
  // the rows).
  IRBuilder<> PB(L ? L->getLoopPreheader()->getTerminator() : InsertPt);
  Value *Cols[4] = {};
  if (Matrix) {
    // The rows of the matrix, transposed.
    Value *MRows[4];
    for (unsigned I = 0; I < 4; ++I)
      MRows[I] = PB.CreateAlignedLoad(
          VecTy, PB.CreateConstInBoundsGEP1_32(PB.getInt8Ty(), Matrix, 16 * I),
          Align(16), "matvec.row");
    Value *Lo01 = PB.CreateShuffleVector(MRows[0], MRows[1], {0, 4, 1, 5});
    Value *Hi01 = PB.CreateShuffleVector(MRows[0], MRows[1], {2, 6, 3, 7});
    Value *Lo23 = PB.CreateShuffleVector(MRows[2], MRows[3], {0, 4, 1, 5});
    Value *Hi23 = PB.CreateShuffleVector(MRows[2], MRows[3], {2, 6, 3, 7});
    Cols[0] = PB.CreateShuffleVector(Lo01, Lo23, {0, 1, 4, 5});
    Cols[1] = PB.CreateShuffleVector(Lo01, Lo23, {2, 3, 6, 7});
    Cols[2] = PB.CreateShuffleVector(Hi01, Hi23, {0, 1, 4, 5});
    Cols[3] = PB.CreateShuffleVector(Hi01, Hi23, {2, 3, 6, 7});
  } else {
    for (unsigned J = 0; J < 4; ++J) {
      if (!Used[J])
        continue;
      SmallVector<Value *, 4> Lanes(4);
      for (auto [I, R] : enumerate(Rows))
        Lanes[I] = R.Coef[J];
      Cols[J] = buildVector(PB, Lanes, PB.getFloatTy());
    }
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
  Value *Vec = FromVector
                   ? Base
                   : B.CreateAlignedLoad(VecTy, Base, Align(16), "matvec.v");
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
    // Not part of a larger sum, also as a sum times a constant in it.
    if (I.hasOneUse()) {
      auto *U = cast<Instruction>(*I.user_begin());
      if (U->getParent() == BB && U->getOpcode() == Instruction::FMul &&
          U->hasOneUse() && any_of(U->operands(), [](const Use &Op) {
            return isa<ConstantFP>(Op);
          }))
        U = cast<Instruction>(*U->user_begin());
      if (U->getParent() == BB && U->getOpcode() == Instruction::FAdd)
        continue;
    }
    // In a loop the vector is what varies. Outside a loop either operand of
    // a product may be the vector: each base of a load in the products.
    SmallSetVector<Value *, 4> Bases;
    if (L) {
      Bases.insert(nullptr);
    } else {
      SmallVector<Value *, 8> Work = {&I};
      while (!Work.empty() && Bases.size() < 4) {
        auto *W = dyn_cast<Instruction>(Work.pop_back_val());
        if (!W || W->getParent() != BB)
          continue;
        if (W->getOpcode() == Instruction::FAdd) {
          Work.append(W->op_begin(), W->op_end());
          continue;
        }
        if (W->getOpcode() != Instruction::FMul)
          continue;
        for (Value *Op : W->operands()) {
          int64_t Offset;
          if (auto *LI = dyn_cast<LoadInst>(Op))
            Bases.insert(GetPointerBaseWithConstantOffset(
                LI->getPointerOperand(), Offset, DL));
        }
      }
    }
    for (Value *Start : Bases) {
      Row R;
      R.Root = &I;
      Value *Base = Start;
      if (!collect(R, &I, Base, 0) || !Base || R.NumTerms < 2)
        continue;
      auto &Rows = Groups[Base];
      if (Rows.size() < 4)
        Rows.push_back(R);
    }
  }

  // Outside a loop a row can be in several groups: once rewritten, its
  // instructions are gone.
  bool Changed = false;
  SmallPtrSet<Instruction *, 8> Done;
  for (auto &[Base, Rows] : Groups) {
    if (Rows.size() < 2 ||
        any_of(Rows, [&](const Row &R) { return Done.contains(R.Root); }))
      continue;
    if (rewrite(Base, Rows)) {
      Changed = true;
      for (Row &R : Rows)
        Done.insert(R.Root);
    }
  }
  return Changed;
}

namespace {
/// The value of an expression of a block at d = 0, or null: Zero stands for
/// a value that is 0.
struct AtZero {
  Value *V = nullptr;
  bool Zero = false;
};
} // namespace

/// \p V with \p D = 0, for the multiplies, additions and subtractions of
/// block \p T.
static AtZero evalAtZero(Value *V, Value *D, BasicBlock *T) {
  if (V == D)
    return {nullptr, true};
  auto *I = dyn_cast<Instruction>(V);
  if (!I || I->getParent() != T)
    return {V, false};
  if (!isa<BinaryOperator>(I))
    return {};
  AtZero A = evalAtZero(I->getOperand(0), D, T);
  AtZero B = evalAtZero(I->getOperand(1), D, T);
  switch (I->getOpcode()) {
  case Instruction::FMul:
    if (A.Zero || B.Zero)
      return {nullptr, true};
    return {};
  case Instruction::FAdd:
    if (A.Zero)
      return B;
    if (B.Zero)
      return A;
    return {};
  case Instruction::FSub:
    if (B.Zero)
      return A;
    return {};
  default:
    return {};
  }
}

/// The conditional sums of the file comment: a block BB that branches on
/// d > 0 (fcmp ogt d, 0.0) to a block T of fast-math multiplies, additions
/// and subtractions that falls through to the other successor M, where each
/// phi gets from T a value that is the one from BB at d = 0.
static bool speculateConditionalSums(Function &F, DominatorTree &DT,
                                     LoopInfo &LI, ScalarEvolution &SE) {
  bool Changed = false;
  // The blocks with a conditional branch; T, once deleted, is not one.
  SmallVector<BasicBlock *, 16> Blocks;
  SmallPtrSet<BasicBlock *, 8> Deleted;
  for (BasicBlock &BB : F)
    if (isa<CondBrInst>(BB.getTerminator()))
      Blocks.push_back(&BB);
  DomTreeUpdater DTU(DT, DomTreeUpdater::UpdateStrategy::Eager);
  for (BasicBlock *BBP : Blocks) {
    // A merged block ends in the branch of the next one.
    while (!Deleted.contains(BBP)) {
      BasicBlock &BB = *BBP;
      auto *Br = dyn_cast<CondBrInst>(BB.getTerminator());
      if (!Br)
        break;
      auto *Cmp = dyn_cast<FCmpInst>(Br->getCondition());
      const APFloat *Zero;
      if (!Cmp || !match(Cmp->getOperand(1), m_APFloat(Zero)) ||
          !Zero->isZero())
        break;
      BasicBlock *T, *M;
      if (Cmp->getPredicate() == FCmpInst::FCMP_OGT)
        T = Br->getSuccessor(0), M = Br->getSuccessor(1);
      else if (Cmp->getPredicate() == FCmpInst::FCMP_OLE)
        T = Br->getSuccessor(1), M = Br->getSuccessor(0);
      else
        break;
      Value *D = Cmp->getOperand(0);
      auto *TBr = dyn_cast<UncondBrInst>(T->getTerminator());
      if (T == M || T->getSinglePredecessor() != &BB || !TBr ||
          TBr->getSuccessor() != M || !D->getType()->isFloatTy())
        break;
      bool OK = true;
      for (Instruction &I : *T) {
        if (&I == TBr)
          break;
        auto *BO = dyn_cast<BinaryOperator>(&I);
        FastMathFlags FMF = BO && isa<FPMathOperator>(BO)
                                ? BO->getFastMathFlags()
                                : FastMathFlags();
        if (!BO ||
            (BO->getOpcode() != Instruction::FMul &&
             BO->getOpcode() != Instruction::FAdd &&
             BO->getOpcode() != Instruction::FSub) ||
            !FMF.noNaNs() || !FMF.noInfs() || !FMF.noSignedZeros() ||
            any_of(I.users(), [&](User *U) {
              auto *UI = cast<Instruction>(U);
              return UI->getParent() != T &&
                     !(isa<PHINode>(UI) && UI->getParent() == M);
            })) {
          OK = false;
          break;
        }
      }
      for (PHINode &Phi : M->phis()) {
        int TIdx = Phi.getBasicBlockIndex(T),
            BIdx = Phi.getBasicBlockIndex(&BB);
        AtZero Z = TIdx < 0 || BIdx < 0
                       ? AtZero()
                       : evalAtZero(Phi.getIncomingValue(TIdx), D, T);
        if (Z.Zero || !Z.V || Z.V != Phi.getIncomingValue(BIdx)) {
          OK = false;
          break;
        }
      }
      if (!OK)
        break;

      // max(d, 0) for d in T, T's instructions in BB, and BB to M.
      IRBuilder<> B(Br);
      FastMathFlags FMF;
      FMF.setNoNaNs();
      FMF.setNoInfs();
      FMF.setNoSignedZeros();
      Value *Max = B.CreateBinaryIntrinsic(
          Intrinsic::maxnum, D, ConstantFP::getZero(D->getType()), FMF, "dpos");
      for (Instruction &I : make_early_inc_range(*T)) {
        if (&I == TBr)
          break;
        I.moveBefore(Br->getIterator());
        I.replaceUsesOfWith(D, Max);
      }
      for (PHINode &Phi : M->phis()) {
        Phi.setIncomingValueForBlock(&BB, Phi.getIncomingValueForBlock(T));
        Phi.removeIncomingValue(T, /*DeletePHIIfEmpty=*/false);
      }
      UncondBrInst::Create(M, Br->getIterator());
      Br->eraseFromParent();
      RecursivelyDeleteTriviallyDeadInstructions(Cmp);
      if (Loop *L = LI.getLoopFor(T)) {
        LI.removeBlock(T);
        SE.forgetLoop(L);
      }
      // M has BB as a predecessor, so T dominates nothing.
      DT.eraseNode(T);
      Deleted.insert(T);
      T->eraseFromParent();
      Changed = true;
      // M after BB, as one block.
      if (M->getSinglePredecessor() != &BB)
        break;
      FoldSingleEntryPHINodes(M);
      Deleted.insert(M);
      MergeBlockIntoPredecessor(M, &DTU, &LI);
    }
  }
  return Changed;
}

PreservedAnalyses MipsR5900VU0MatVecPass::run(Function &F,
                                              FunctionAnalysisManager &AM) {
  const MipsSubtarget *ST = TM.getSubtargetImpl(F);
  if (!EnableMatVec || !ST->isR5900())
    return PreservedAnalyses::all();

  auto &LI = AM.getResult<LoopAnalysis>(F);
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &AC = AM.getResult<AssumptionAnalysis>(F);
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
  auto &AA = AM.getResult<AAManager>(F);
  // Also for the FPU, which has MAX.S.
  bool CFGChanged = speculateConditionalSums(F, DT, LI, SE);
  if (!ST->hasVU0())
    return CFGChanged ? PreservedAnalyses::none() : PreservedAnalyses::all();
  bool Changed = CFGChanged;
  for (Loop *L : LI.getLoopsInPreorder()) {
    if (!L->getLoopPreheader())
      continue;
    for (BasicBlock *BB : L->blocks())
      if (LI.getLoopFor(BB) == L)
        // The results of one rewrite can be the vector of the next one.
        for (unsigned K = 0;
             K != 3 && MatVec(F.getDataLayout(), L, BB, SE, DT, AC, AA).run();
             ++K)
          Changed = true;
  }
  for (BasicBlock &BB : F)
    if (!LI.getLoopFor(&BB))
      Changed |= MatVec(F.getDataLayout(), nullptr, &BB, SE, DT, AC, AA).run();
  if (!Changed)
    return PreservedAnalyses::all();
  if (CFGChanged)
    return PreservedAnalyses::none();
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
