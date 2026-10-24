//===-- VUNarrowInt.cpp - 16-bit integers ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The VU's integer registers are 16 bits. HLSL's int and uint are 32 bits:
// scalar integer operations wider than 16 bits are done in 16 bits instead,
// so a counter, an index or a constant from a uniform keeps working for the
// values the VU can address. A 32-bit value that is stored or converted is
// the 16-bit one sign-extended; a loaded one is the low half of its field.
//
// The VU has no integer vector unit: an integer vector operation is done
// lane by lane first (the vector is built again for the users that need
// it), so that its lanes are narrowed like the scalars.
//
//===----------------------------------------------------------------------===//

#include "VU.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

#define DEBUG_TYPE "vu-narrow-int"

namespace {
class VUNarrowInt : public FunctionPass {
public:
  static char ID;
  VUNarrowInt() : FunctionPass(ID) {}
  StringRef getPassName() const override { return "VU 16-bit integers"; }
  bool runOnFunction(Function &F) override;

private:
  DenseMap<Value *, Value *> Narrow;
  Type *I16 = nullptr;
  Value *get16(Value *V);
  Value *narrow(Instruction &I);
  bool scalarizeVectors(Function &F);
};
} // namespace

char VUNarrowInt::ID = 0;

INITIALIZE_PASS(VUNarrowInt, DEBUG_TYPE, "VU 16-bit integers", false, false)

FunctionPass *llvm::createVUNarrowIntPass() { return new VUNarrowInt(); }

static bool isWide(Type *T) {
  return T->isIntegerTy() && T->getIntegerBitWidth() > 16;
}

/// A vector of integers (not of i1: the masks of comparisons are vectors of
/// floats on the VU).
static FixedVectorType *intVector(Type *T) {
  auto *VT = dyn_cast<FixedVectorType>(T);
  return VT && VT->getElementType()->isIntegerTy() &&
                 VT->getElementType()->getIntegerBitWidth() >= 8
             ? VT
             : nullptr;
}

/// Integer vector operations lane by lane, and the lanes that are extracted
/// found where they come from:
///  - a lane of a vector of lanes twice as wide (HLSL's uint4, <4 x i32>,
///    read as <8 x i16>): the low or the high half of the wide lane;
///  - a lane of a loaded vector: a load of the lane (ILW once narrowed);
///  - a lane of a vector that is built (by the scalarized operations): the
///    scalar.
bool VUNarrowInt::scalarizeVectors(Function &F) {
  bool Changed = false;
  SmallVector<Instruction *, 16> Ops;
  for (Instruction &I : instructions(F)) {
    if (isa<BinaryOperator, SelectInst>(I) && intVector(I.getType()))
      Ops.push_back(&I);
    else if (isa<ICmpInst>(I) && intVector(I.getOperand(0)->getType()))
      Ops.push_back(&I);
    else if (isa<ZExtInst, SExtInst, TruncInst>(I) && intVector(I.getType()))
      Ops.push_back(&I);
  }
  for (Instruction *I : Ops) {
    auto *VT = cast<FixedVectorType>(I->getType());
    IRBuilder<> B(I);
    Value *V = PoisonValue::get(VT);
    for (unsigned K = 0, N = VT->getNumElements(); K != N; ++K) {
      auto Lane = [&](Value *X) {
        return X->getType()->isVectorTy() ? B.CreateExtractElement(X, K) : X;
      };
      Value *R;
      if (auto *BO = dyn_cast<BinaryOperator>(I))
        R = B.CreateBinOp(BO->getOpcode(), Lane(BO->getOperand(0)),
                          Lane(BO->getOperand(1)));
      else if (auto *C = dyn_cast<ICmpInst>(I))
        R = B.CreateICmp(C->getPredicate(), Lane(C->getOperand(0)),
                         Lane(C->getOperand(1)));
      else if (auto *S = dyn_cast<SelectInst>(I))
        R = B.CreateSelect(Lane(S->getCondition()), Lane(S->getTrueValue()),
                           Lane(S->getFalseValue()));
      else
        R = B.CreateCast(cast<CastInst>(I)->getOpcode(), Lane(I->getOperand(0)),
                         VT->getElementType());
      V = B.CreateInsertElement(V, R, K);
    }
    V->takeName(I);
    I->replaceAllUsesWith(V);
    I->eraseFromParent();
    Changed = true;
  }

  SmallVector<ExtractElementInst *, 16> Work;
  for (Instruction &I : instructions(F))
    if (auto *EE = dyn_cast<ExtractElementInst>(&I))
      Work.push_back(EE);
  while (!Work.empty()) {
    ExtractElementInst *EE = Work.pop_back_val();
    auto *Idx = dyn_cast<ConstantInt>(EE->getIndexOperand());
    if (!Idx || !EE->getType()->isIntegerTy())
      continue;
    unsigned K = Idx->getZExtValue(), W = EE->getType()->getIntegerBitWidth();
    Value *Src = EE->getVectorOperand(), *New = nullptr;
    if (auto *BC = dyn_cast<BitCastInst>(Src)) {
      auto *From = intVector(BC->getOperand(0)->getType());
      if (From && From->getElementType()->getIntegerBitWidth() == 2 * W) {
        IRBuilder<> B(EE);
        auto *L = cast<ExtractElementInst>(
            B.CreateExtractElement(BC->getOperand(0), K / 2));
        Work.push_back(L);
        Value *H = K & 1 ? B.CreateLShr(L, W) : static_cast<Value *>(L);
        New = B.CreateTrunc(H, EE->getType());
      }
    } else if (auto *LI = dyn_cast<LoadInst>(Src); LI && LI->isSimple()) {
      // At the load: no store comes between.
      IRBuilder<> B(LI->getNextNode());
      Value *P = B.CreateConstInBoundsGEP1_32(
          B.getInt8Ty(), LI->getPointerOperand(), K * W / 8);
      LoadInst *L = B.CreateLoad(EE->getType(), P, EE->getName());
      L->copyMetadata(*LI, {LLVMContext::MD_alias_scope,
                            LLVMContext::MD_noalias, LLVMContext::MD_tbaa});
      New = L;
    }
    if (!New)
      continue;
    EE->replaceAllUsesWith(New);
    EE->eraseFromParent();
    Changed = true;
  }
  if (Changed)
    for (BasicBlock &BB : F)
      SimplifyInstructionsInBlock(&BB);
  return Changed;
}

/// \p V as an i16: its narrow version, or its low 16 bits.
Value *VUNarrowInt::get16(Value *V) {
  if (V->getType() == I16)
    return V;
  if (auto *C = dyn_cast<ConstantInt>(V))
    return ConstantInt::get(I16, C->getValue().trunc(16));
  if (isa<UndefValue>(V))
    return isa<PoisonValue>(V) ? PoisonValue::get(I16) : UndefValue::get(I16);
  if (Value *N = Narrow.lookup(V))
    return N;
  // Something that stays wide (or is narrower): its low bits.
  IRBuilder<> B(V->getContext());
  if (auto *I = dyn_cast<Instruction>(V)) {
    if (isa<PHINode>(I))
      B.SetInsertPoint(I->getParent()->getFirstNonPHIIt());
    else
      B.SetInsertPoint(std::next(I->getIterator()));
  } else {
    BasicBlock &Entry = cast<Argument>(V)->getParent()->getEntryBlock();
    B.SetInsertPoint(Entry.getFirstInsertionPt());
  }
  Value *T = V->getType()->getIntegerBitWidth() > 16 ? B.CreateTrunc(V, I16)
                                                     : B.CreateSExt(V, I16);
  Narrow[V] = T;
  return T;
}

/// The 16-bit version of \p I, or null if it keeps its type.
Value *VUNarrowInt::narrow(Instruction &I) {
  IRBuilder<> B(&I);
  Type *T = I.getType();
  if (auto *BO = dyn_cast<BinaryOperator>(&I); BO && isWide(T))
    return B.CreateBinOp(BO->getOpcode(), get16(BO->getOperand(0)),
                         get16(BO->getOperand(1)), I.getName());
  if (auto *C = dyn_cast<ICmpInst>(&I);
      C && isWide(C->getOperand(0)->getType()))
    return B.CreateICmp(C->getPredicate(), get16(C->getOperand(0)),
                        get16(C->getOperand(1)), I.getName());
  if (auto *S = dyn_cast<SelectInst>(&I); S && isWide(T))
    return B.CreateSelect(S->getCondition(), get16(S->getTrueValue()),
                          get16(S->getFalseValue()), I.getName());
  if (auto *C = dyn_cast<CastInst>(&I)) {
    Value *Op = C->getOperand(0);
    Type *OpT = Op->getType();
    switch (C->getOpcode()) {
    case Instruction::ZExt:
    case Instruction::SExt:
      if (!isWide(T))
        return nullptr;
      if (OpT == I16)
        return Op;
      if (OpT->getIntegerBitWidth() < 16)
        return B.CreateCast(C->getOpcode(), Op, I16, I.getName());
      return get16(Op);
    case Instruction::Trunc:
      if (!isWide(OpT))
        return nullptr;
      if (T == I16)
        return get16(Op);
      if (isWide(T))
        return get16(Op);
      return B.CreateTrunc(get16(Op), T, I.getName());
    case Instruction::FPToSI:
    case Instruction::FPToUI:
      // FTOI0 and the low half of the field.
      if (!isWide(T))
        return nullptr;
      return B.CreateFPToSI(Op, I16, I.getName());
    case Instruction::SIToFP:
    case Instruction::UIToFP:
      if (!isWide(OpT))
        return nullptr;
      return B.CreateCast(C->getOpcode(), get16(Op), T, I.getName());
    default:
      return nullptr;
    }
  }
  if (auto *LI = dyn_cast<LoadInst>(&I); LI && isWide(T))
    return B.CreateLoad(I16, LI->getPointerOperand(), I.getName());
  if (auto *II = dyn_cast<IntrinsicInst>(&I); II && isWide(T)) {
    switch (II->getIntrinsicID()) {
    case Intrinsic::smin:
    case Intrinsic::smax:
    case Intrinsic::umin:
    case Intrinsic::umax:
      return B.CreateBinaryIntrinsic(II->getIntrinsicID(),
                                     get16(II->getArgOperand(0)),
                                     get16(II->getArgOperand(1)));
    case Intrinsic::abs:
      return B.CreateBinaryIntrinsic(
          Intrinsic::abs, get16(II->getArgOperand(0)), II->getArgOperand(1));
    default:
      return nullptr;
    }
  }
  return nullptr;
}

bool VUNarrowInt::runOnFunction(Function &F) {
  I16 = Type::getInt16Ty(F.getContext());
  Narrow.clear();
  bool Vectors = scalarizeVectors(F);
  // Phis first, so that every value has its narrow version when it is used.
  SmallVector<std::pair<PHINode *, PHINode *>, 8> Phis;
  for (Instruction &I : instructions(F))
    if (auto *P = dyn_cast<PHINode>(&I); P && isWide(P->getType())) {
      PHINode *N = PHINode::Create(I16, P->getNumIncomingValues(), P->getName(),
                                   P->getIterator());
      Narrow[P] = N;
      Phis.push_back({P, N});
    }
  SmallVector<Instruction *, 32> Old;
  ReversePostOrderTraversal<Function *> RPOT(&F);
  for (BasicBlock *BB : RPOT)
    for (Instruction &I : make_early_inc_range(*BB)) {
      if (isa<PHINode>(I) || Narrow.count(&I))
        continue;
      Value *N = narrow(I);
      if (!N)
        continue;
      Narrow[&I] = N;
      Old.push_back(&I);
    }
  for (auto [P, N] : Phis) {
    for (unsigned K = 0, E = P->getNumIncomingValues(); K != E; ++K)
      N->addIncoming(get16(P->getIncomingValue(K)), P->getIncomingBlock(K));
    Old.push_back(P);
  }
  if (Old.empty())
    return Vectors;

  // The users that keep the wide type get the narrow value sign-extended.
  for (Instruction *I : Old) {
    Value *N = Narrow[I];
    if (N->getType() == I->getType()) {
      I->replaceAllUsesWith(N);
      continue;
    }
    for (Use &U : make_early_inc_range(I->uses())) {
      auto *User = cast<Instruction>(U.getUser());
      if (Narrow.count(User))
        continue;
      Instruction *At = User;
      if (auto *P = dyn_cast<PHINode>(User))
        At = P->getIncomingBlock(U)->getTerminator();
      IRBuilder<> B(At);
      U.set(isWide(I->getType()) ? B.CreateSExt(N, I->getType())
                                 : static_cast<Value *>(N));
    }
  }
  for (Instruction *I : Old)
    I->replaceAllUsesWith(PoisonValue::get(I->getType()));
  for (Instruction *I : Old)
    I->eraseFromParent();
  for (BasicBlock &BB : F)
    SimplifyInstructionsInBlock(&BB);
  return true;
}
