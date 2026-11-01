//===- MipsR5900Realign.cpp - R5900 unaligned vector loads ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A vectorized loop often loads vectors at constant byte offsets from a
// 16-byte aligned pointer, such as in[i + k] for the taps k of a filter with
// i a multiple of the vector length. The R5900 lowers such an unaligned load
// as the two aligned quadwords around it and a QFSRV by the constant offset
// (MipsSETargetLowering::lowerLOAD), which nearby loads share, but the
// SelectionDAG only sees the alignment of the pointer when it is computed in
// the same block. This pass, which runs last in the optimization pipeline,
// rewrites the pointer of each such load as an offset from llvm.ptrmask(p,
// -16) of the aligned pointer p: the same address, with the alignment in the
// DAG (an AND). Loads at a multiple of 16 bytes from it are made aligned.
//
// It also writes the 64 byte stores of an 8x8 transpose, out[8 * y + x] =
// v_x[y] for 8 vectors v_x of 8 bytes, as the transpose in vector registers
// (MMI PSLLH and POR, PEXTLH/PEXTUH, PEXTLW/PEXTUW) and one 64-byte store:
// SLP vectorizes the planar to chunky conversion of an 8x8 tile across its
// rows that way, and the stores of single lanes go through the stack.
//
// And it writes a mask of 16 compares of 32-bit values as bytes, zext <16 x
// i1> to <16 x i8>, as the truncation of the mask as words (PPACH, PPACB):
// the compares give word masks, which type legalization otherwise narrows
// one lane at a time through the stack.
//
//===----------------------------------------------------------------------===//

#include "MipsR5900Realign.h"
#include "MipsSubtarget.h"
#include "MipsTargetMachine.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;
using namespace llvm::PatternMatch;

#define DEBUG_TYPE "mips-r5900-realign"

STATISTIC(NumRealigned, "Number of vector loads rebased on an aligned pointer");
STATISTIC(NumAligned, "Number of vector loads found 16-byte aligned");
STATISTIC(NumGrouped, "Number of sums of products grouped by shift amount");
STATISTIC(NumTransposed, "Number of 8x8 byte transposes stored as vectors");
STATISTIC(NumNarrowed, "Number of word compare masks narrowed to bytes");

static cl::opt<bool>
    EnableRealign("mips-r5900-realign", cl::init(true), cl::Hidden,
                  cl::desc("Rebase R5900 unaligned vector loads on an "
                           "aligned pointer"));

static cl::opt<unsigned> MaxShifts(
    "mips-r5900-realign-max-shifts", cl::init(2), cl::Hidden,
    cl::desc("Most shift amounts of the rebased loads of a pointer in a "
             "block"));

/// Whether \p LI is a term of a v8i32 sum of products, sext(load) * x + ...,
/// which the R5900 lowering (performR5900ADDCombine) orders by the shift
/// amounts of the loads, so that the loads of each shift share one MTSAB.
static bool feedsMulAdd(const LoadInst *LI) {
  const Value *V = LI;
  if (!V->hasOneUse() || !isa<SExtInst>(V = V->user_back()))
    return false;
  if (auto *VTy = dyn_cast<FixedVectorType>(V->getType());
      !VTy || VTy->getNumElements() != 8 ||
      !VTy->getElementType()->isIntegerTy(32))
    return false;
  if (!V->hasOneUse() ||
      cast<Instruction>(V = V->user_back())->getOpcode() != Instruction::Mul)
    return false;
  return V->hasOneUse() &&
         cast<Instruction>(V->user_back())->getOpcode() == Instruction::Add;
}

namespace {
/// A load and its shift amount (offset & 15 from an aligned base) and offset.
struct LoadShift {
  int64_t Shift;
  int64_t Off;
};
} // namespace

/// Reassociate each v8i32 sum of products in a block, sext(load) * x + ...,
/// so that the terms whose loads have the same shift amount (\p Shifts) are
/// next to each other, and move each term (the load, sext and multiply) to
/// its add: the SelectionDAG keeps that order, and the QFSRVs of each shift
/// then share one MTSAB (MipsR5900SARedundant).
static bool groupMulAdds(Function &F,
                         const DenseMap<LoadInst *, LoadShift> &Shifts) {
  auto IsV8I32Add = [](const Value *V) {
    auto *I = dyn_cast<Instruction>(V);
    auto *VTy = I ? dyn_cast<FixedVectorType>(I->getType()) : nullptr;
    return VTy && I->getOpcode() == Instruction::Add &&
           VTy->getNumElements() == 8 && VTy->getElementType()->isIntegerTy(32);
  };
  SmallVector<Instruction *, 8> Roots;
  for (Instruction &I : instructions(F))
    if (IsV8I32Add(&I) && !(I.hasOneUse() && IsV8I32Add(I.user_back()) &&
                            I.user_back()->getParent() == I.getParent()))
      Roots.push_back(&I);

  bool Changed = false;
  for (Instruction *Root : Roots) {
    BasicBlock *BB = Root->getParent();
    struct Term {
      Value *V;
      LoadInst *LI = nullptr;
      Instruction *Ext = nullptr;
      LoadShift Key = {-1, 0};
    };
    SmallVector<Term, 32> Terms;
    SmallVector<Instruction *, 32> Adds;
    SmallVector<Value *, 32> Work = {Root};
    while (!Work.empty()) {
      Value *V = Work.pop_back_val();
      auto *I = dyn_cast<Instruction>(V);
      if (IsV8I32Add(V) && I->getParent() == BB &&
          (I == Root || I->hasOneUse())) {
        Adds.push_back(I);
        Work.push_back(I->getOperand(1));
        Work.push_back(I->getOperand(0));
        continue;
      }
      Term T{V};
      if (I && I->getOpcode() == Instruction::Mul && I->hasOneUse() &&
          I->getParent() == BB)
        for (Value *Op : I->operands())
          if (auto *Ext = dyn_cast<SExtInst>(Op))
            if (auto *LI = dyn_cast<LoadInst>(Ext->getOperand(0)))
              if (auto It = Shifts.find(LI);
                  It != Shifts.end() && Ext->hasOneUse() &&
                  Ext->getParent() == BB && LI->hasOneUse() &&
                  LI->getParent() == BB) {
                T.LI = LI;
                T.Ext = Ext;
                T.Key = It->second;
                break;
              }
      Terms.push_back(T);
    }
    auto Less = [](const Term &A, const Term &B) {
      return std::make_pair(A.Key.Shift, A.Key.Off) <
             std::make_pair(B.Key.Shift, B.Key.Off);
    };
    SmallSet<int64_t, 8> Distinct;
    for (const Term &T : Terms)
      if (T.Key.Shift > 0)
        Distinct.insert(T.Key.Shift);
    if (Distinct.size() < 2 || llvm::is_sorted(Terms, Less))
      continue;
    // The loads move down to the root: nothing in between may write memory.
    Instruction *First = Root;
    for (const Term &T : Terms)
      if (T.LI && T.LI->comesBefore(First))
        First = T.LI;
    if (any_of(make_range(First->getIterator(), Root->getIterator()),
               [](const Instruction &I) { return I.mayWriteToMemory(); }))
      continue;

    llvm::stable_sort(Terms, Less);
    IRBuilder<> B(Root);
    Value *Sum = nullptr;
    for (const Term &T : Terms) {
      if (T.LI) {
        T.LI->moveBefore(Root->getIterator());
        T.Ext->moveBefore(Root->getIterator());
        cast<Instruction>(T.V)->moveBefore(Root->getIterator());
      }
      // Without nsw: the partial sums are other ones now.
      Sum = Sum ? B.CreateAdd(T.V, Sum) : T.V;
    }
    Sum->takeName(Root);
    Root->replaceAllUsesWith(Sum);
    for (Instruction *A : Adds)
      A->dropAllReferences();
    for (Instruction *A : Adds)
      A->eraseFromParent();
    ++NumGrouped;
    Changed = true;
  }
  return Changed;
}

/// The 8x8 byte transposes stored in \p BB (see the file comment).
static bool transposeByteStores(BasicBlock &BB, const DataLayout &DL) {
  struct Lane {
    StoreInst *SI;
    Value *Vec;
    unsigned Idx;
  };
  MapVector<Value *, DenseMap<int64_t, Lane>> Groups;
  for (Instruction &I : BB) {
    auto *SI = dyn_cast<StoreInst>(&I);
    if (!SI || !SI->isSimple())
      continue;
    auto *EE = dyn_cast<ExtractElementInst>(SI->getValueOperand());
    auto *VTy =
        EE ? dyn_cast<FixedVectorType>(EE->getVectorOperandType()) : nullptr;
    auto *Idx = EE ? dyn_cast<ConstantInt>(EE->getIndexOperand()) : nullptr;
    if (!VTy || !Idx || VTy->getNumElements() != 8 ||
        !VTy->getElementType()->isIntegerTy(8))
      continue;
    APInt Off(DL.getIndexTypeSizeInBits(SI->getPointerOperandType()), 0);
    Value *Base = SI->getPointerOperand()->stripAndAccumulateConstantOffsets(
        DL, Off, /*AllowNonInbounds=*/true);
    Groups[Base].insert(
        {Off.getSExtValue(),
         {SI, EE->getVectorOperand(), unsigned(Idx->getZExtValue())}});
  }

  bool Changed = false;
  for (auto &[Base, Lanes] : Groups) {
    if (Lanes.size() != 64)
      continue;
    int64_t Start = INT64_MAX;
    for (auto &[Off, L] : Lanes)
      Start = std::min(Start, Off);
    // Byte 8y + x is lane y of v_x.
    Value *Vecs[8] = {};
    bool OK = true;
    StoreInst *First = nullptr, *Last = nullptr;
    for (int64_t B = 0; B != 64 && OK; ++B) {
      auto It = Lanes.find(Start + B);
      if (It == Lanes.end() || It->second.Idx != B / 8) {
        OK = false;
        break;
      }
      Value *&V = Vecs[B % 8];
      if (V && V != It->second.Vec)
        OK = false;
      V = It->second.Vec;
      StoreInst *SI = It->second.SI;
      if (!First || SI->comesBefore(First))
        First = SI;
      if (!Last || Last->comesBefore(SI))
        Last = SI;
    }
    if (!OK)
      continue;
    // The stores move down to the last one: no other memory access between.
    SmallPtrSet<Instruction *, 32> Stores;
    for (auto &[Off, L] : Lanes)
      Stores.insert(L.SI);
    if (any_of(make_range(First->getIterator(), Last->getIterator()),
               [&](Instruction &I) {
                 return I.mayReadOrWriteMemory() && !Stores.contains(&I);
               }))
      continue;

    IRBuilder<> B(Last);
    auto *V16 = FixedVectorType::get(B.getInt16Ty(), 8);
    Value *W[4];
    for (unsigned P = 0; P != 4; ++P)
      W[P] = B.CreateOr(B.CreateZExt(Vecs[2 * P], V16),
                        B.CreateShl(B.CreateZExt(Vecs[2 * P + 1], V16), 8));
    SmallVector<int, 16> Interleave;
    for (int K = 0; K != 8; ++K)
      Interleave.append({K, K + 8});
    auto *V32 = FixedVectorType::get(B.getInt32Ty(), 8);
    Value *X0 =
        B.CreateBitCast(B.CreateShuffleVector(W[0], W[1], Interleave), V32);
    Value *X1 =
        B.CreateBitCast(B.CreateShuffleVector(W[2], W[3], Interleave), V32);
    Value *Rows = B.CreateShuffleVector(X0, X1, Interleave, "transpose");
    StoreInst *StartSI = Lanes.find(Start)->second.SI;
    Value *Ptr = StartSI->getPointerOperand();
    B.CreateAlignedStore(Rows, Ptr, StartSI->getAlign());
    SmallVector<WeakTrackingVH, 128> Dead;
    for (Instruction *SI : Stores) {
      Dead.push_back(cast<StoreInst>(SI)->getValueOperand());
      // Keep the pointer of the new store.
      if (SI != StartSI)
        Dead.push_back(SI->getOperand(StoreInst::getPointerOperandIndex()));
      else
        SI->setOperand(StoreInst::getPointerOperandIndex(),
                       PoisonValue::get(Ptr->getType()));
      SI->eraseFromParent();
    }
    RecursivelyDeleteTriviallyDeadInstructions(Dead);
    ++NumTransposed;
    Changed = true;
  }
  return Changed;
}

/// The i1 vector \p V as a mask of words (all ones or zero): a compare of
/// 32-bit elements sign extended, and an and, or or a select that is one
/// (select a, b, false; select a, true, b) of such. Null if it is not.
static Value *widenWordMask(Value *V, IRBuilderBase &B,
                            DenseMap<Value *, Value *> &Widened,
                            unsigned Depth = 0) {
  if (Value *W = Widened.lookup(V))
    return W;
  auto *VTy = cast<FixedVectorType>(V->getType());
  auto *WTy = FixedVectorType::get(B.getInt32Ty(), VTy->getNumElements());
  Value *W = nullptr;
  if (auto *C = dyn_cast<CmpInst>(V)) {
    if (C->getOperand(0)->getType()->getScalarSizeInBits() != 32)
      return nullptr;
    W = B.CreateSExt(C, WTy);
  } else if (Depth > 8) {
    return nullptr;
  } else if (auto *Sel = dyn_cast<SelectInst>(V)) {
    Value *Cond = Sel->getCondition(), *T = Sel->getTrueValue(),
          *F = Sel->getFalseValue();
    bool And = match(F, m_Zero()), Or = match(T, m_One());
    if (!And && !Or)
      return nullptr;
    Value *WA = widenWordMask(Cond, B, Widened, Depth + 1);
    Value *WB =
        WA ? widenWordMask(And ? T : F, B, Widened, Depth + 1) : nullptr;
    if (!WB)
      return nullptr;
    W = And ? B.CreateAnd(WA, WB) : B.CreateOr(WA, WB);
  } else if (auto *BO = dyn_cast<BinaryOperator>(V);
             BO && (BO->getOpcode() == Instruction::And ||
                    BO->getOpcode() == Instruction::Or)) {
    Value *WA = widenWordMask(BO->getOperand(0), B, Widened, Depth + 1);
    Value *WB =
        WA ? widenWordMask(BO->getOperand(1), B, Widened, Depth + 1) : nullptr;
    if (!WB)
      return nullptr;
    W = B.CreateBinOp(BO->getOpcode(), WA, WB);
  } else {
    return nullptr;
  }
  Widened[V] = W;
  return W;
}

/// The byte masks of 16 word compares (see the file comment): the mask is
/// computed as words, and each zero extension of it is the low bit of that,
/// truncated for bytes.
static bool narrowWordMasks(Function &F) {
  bool Changed = false;
  for (Instruction &I : make_early_inc_range(instructions(F))) {
    auto *Z = dyn_cast<ZExtInst>(&I);
    auto *DTy = Z ? dyn_cast<FixedVectorType>(Z->getType()) : nullptr;
    if (!DTy || DTy->getNumElements() != 16 ||
        !DTy->getElementType()->isIntegerTy(8) ||
        !Z->getOperand(0)->getType()->isIntOrIntVectorTy(1))
      continue;
    Value *M = Z->getOperand(0);
    auto *MI = dyn_cast<Instruction>(M);
    if (!MI)
      continue;
    IRBuilder<> B(MI->getNextNode());
    DenseMap<Value *, Value *> Widened;
    Value *W = widenWordMask(M, B, Widened);
    if (!W) {
      // The partial widening is dead.
      SmallVector<WeakTrackingVH, 8> Dead(llvm::make_second_range(Widened));
      RecursivelyDeleteTriviallyDeadInstructions(Dead);
      continue;
    }
    // Every zero extension of the mask: the low bit of each word.
    Value *Bits = B.CreateAnd(W, ConstantInt::get(W->getType(), 1));
    for (User *U : make_early_inc_range(M->users())) {
      auto *E = dyn_cast<ZExtInst>(U);
      if (!E)
        continue;
      unsigned Bits32 = E->getType()->getScalarSizeInBits();
      IRBuilder<> EB(E);
      Value *R = Bits32 == 32  ? Bits
                 : Bits32 < 32 ? EB.CreateTrunc(Bits, E->getType())
                               : EB.CreateZExt(Bits, E->getType());
      R->takeName(E);
      E->replaceAllUsesWith(R);
      E->eraseFromParent();
    }
    RecursivelyDeleteTriviallyDeadInstructions(M);
    ++NumNarrowed;
    Changed = true;
  }
  return Changed;
}

PreservedAnalyses MipsR5900RealignPass::run(Function &F,
                                            FunctionAnalysisManager &AM) {
  const MipsSubtarget &ST = TM.getSubtarget<MipsSubtarget>(F);
  if (!EnableRealign || !ST.isR5900())
    return PreservedAnalyses::all();
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &AC = AM.getResult<AssumptionAnalysis>(F);
  const DataLayout &DL = F.getDataLayout();
  Type *IntPtrTy = DL.getIntPtrType(F.getContext());
  // The ptrmask of each aligned base.
  DenseMap<Value *, Value *> Masked;
  bool Changed = false;
  for (BasicBlock &BB : F)
    Changed |= transposeByteStores(BB, DL);
  Changed |= narrowWordMasks(F);

  SmallVector<LoadInst *, 32> Loads;
  for (Instruction &I : instructions(F))
    if (auto *LI = dyn_cast<LoadInst>(&I))
      if (auto *VTy = dyn_cast<FixedVectorType>(LI->getType()))
        if (LI->isSimple() && VTy->getElementType()->isIntegerTy() &&
            DL.getTypeStoreSize(VTy) == 16 && LI->getAlign() < Align(16))
          Loads.push_back(LI);

  // The loads from an aligned base and their offsets, and the shift amounts
  // (offset & 15) of the loads of each base in each block.
  struct Rebase {
    LoadInst *LI;
    Value *Base;
    int64_t Off;
    bool Grouped;
  };
  SmallVector<Rebase, 32> Rebases;
  DenseMap<std::pair<Value *, BasicBlock *>, SmallSet<int64_t, 4>> Shifts;
  DenseMap<LoadInst *, LoadShift> LoadShifts;
  for (LoadInst *LI : Loads) {
    Value *Ptr = LI->getPointerOperand();
    APInt Offset(DL.getIndexTypeSizeInBits(Ptr->getType()), 0);
    Value *Base = Ptr->stripAndAccumulateConstantOffsets(
        DL, Offset, /*AllowNonInbounds=*/true);
    if (getKnownAlignment(Base, DL, LI, &AC, &DT) < Align(16))
      continue;
    int64_t Off = Offset.getSExtValue();
    bool Grouped = feedsMulAdd(LI);
    Rebases.push_back({LI, Base, Off, Grouped});
    if ((Off & 15) && !Grouped)
      Shifts[{Base, LI->getParent()}].insert(Off & 15);
    LoadShifts[LI] = {Off & 15, Off};
  }
  Changed |= groupMulAdds(F, LoadShifts);

  for (auto [LI, Base, Off, Grouped] : Rebases) {
    Value *Ptr = LI->getPointerOperand();
    if (Off % 16 == 0) {
      LI->setAlignment(Align(16));
      ++NumAligned;
      Changed = true;
      continue;
    }
    // Each shift amount is an MTSAB, and the R5900 does not allow one within
    // 3 instructions after a QFSRV; with more than MaxShifts of them in a
    // block, as for the taps of a filter, the nops cost more than the shared
    // quadwords save. The terms of a sum of products are grouped by shift.
    if (!Grouped && Shifts[{Base, LI->getParent()}].size() > MaxShifts)
      continue;
    // The aligned base, after its definition (at the entry for an argument
    // or a constant).
    Value *&M = Masked[Base];
    if (!M) {
      IRBuilder<> B(F.getContext());
      if (auto *I = dyn_cast<Instruction>(Base))
        B.SetInsertPoint(
            I->getInsertionPointAfterDef().value_or(LI->getIterator()));
      else
        B.SetInsertPoint(F.getEntryBlock().getFirstInsertionPt());
      auto *Call = cast<CallInst>(
          B.CreateIntrinsic(Intrinsic::ptrmask, {Base->getType(), IntPtrTy},
                            {Base, ConstantInt::getSigned(IntPtrTy, -16)},
                            nullptr, Base->getName() + ".aligned"));
      Call->addRetAttr(Attribute::getWithAlignment(F.getContext(), Align(16)));
      M = Call;
    }
    IRBuilder<> B(LI);
    LI->setOperand(LoadInst::getPointerOperandIndex(),
                   B.CreatePtrAdd(M, ConstantInt::get(IntPtrTy, Off)));
    RecursivelyDeleteTriviallyDeadInstructions(Ptr);
    ++NumRealigned;
    Changed = true;
  }
  if (!Changed)
    return PreservedAnalyses::all();
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
