//===-- VUSinkFields.cpp - Fields of vectors next to their uses -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A field of a vector, or a splat of it, is free where an FMAC instruction
// reads it by its broadcast field (MULz d, a, b.z), but costs an instruction
// and a register where it is a value of its own (ADDz.x d, vf0, b). LICM
// hoists the fields of loop-invariant vectors out of loops, where instruction
// selection only sees the scalar; this pass makes a copy of them in each
// block that uses them, next to the uses, so the vector stays live instead of
// its fields. It also expands reductions of vectors of floats (see
// expandReductions); both run before instruction selection.
//
//===----------------------------------------------------------------------===//

#include "VU.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Pass.h"

using namespace llvm;
using namespace llvm::PatternMatch;

#define DEBUG_TYPE "vu-sink-fields"

namespace {
class VUSinkFields : public FunctionPass {
public:
  static char ID;
  VUSinkFields() : FunctionPass(ID) {}
  StringRef getPassName() const override { return "VU sink vector fields"; }
  bool runOnFunction(Function &F) override;
};
} // namespace

char VUSinkFields::ID = 0;

INITIALIZE_PASS(VUSinkFields, DEBUG_TYPE, "VU sink vector fields", false, false)

FunctionPass *llvm::createVUSinkFieldsPass() { return new VUSinkFields(); }

/// A field of a vector at a constant index, or a splat of one.
static bool isField(Instruction &I) {
  if (auto *EE = dyn_cast<ExtractElementInst>(&I))
    return isa<ConstantInt>(EE->getIndexOperand());
  if (auto *SV = dyn_cast<ShuffleVectorInst>(&I))
    return SV->isZeroEltSplat() ||
           all_equal(SV->getShuffleMask()) && SV->getShuffleMask()[0] >= 0;
  return false;
}

/// A reduction of a vector of floats as a chain over its fields: the sums of
/// fields are then lane sums (ADDy, ADDz) and the minima and maxima lane
/// operations, instead of the shuffles of the generic expansion.
static bool expandReductions(Function &F) {
  bool Changed = false;
  for (Instruction &I : make_early_inc_range(instructions(F))) {
    auto *II = dyn_cast<IntrinsicInst>(&I);
    if (!II)
      continue;
    Intrinsic::ID ID = II->getIntrinsicID();
    unsigned VecArg;
    Instruction::BinaryOps Op = Instruction::BinaryOpsEnd;
    Intrinsic::ID MinMax = Intrinsic::not_intrinsic;
    switch (ID) {
    case Intrinsic::vector_reduce_fadd:
      VecArg = 1, Op = Instruction::FAdd;
      break;
    case Intrinsic::vector_reduce_fmul:
      VecArg = 1, Op = Instruction::FMul;
      break;
    case Intrinsic::vector_reduce_fmax:
      VecArg = 0, MinMax = Intrinsic::maxnum;
      break;
    case Intrinsic::vector_reduce_fmin:
      VecArg = 0, MinMax = Intrinsic::minnum;
      break;
    default:
      continue;
    }
    Value *V = II->getArgOperand(VecArg);
    auto *VT = dyn_cast<FixedVectorType>(V->getType());
    if (!VT)
      continue;
    IRBuilder<> B(II);
    B.setFastMathFlags(II->getFastMathFlags());
    Value *R = B.CreateExtractElement(V, uint64_t(0));
    for (unsigned K = 1, E = VT->getNumElements(); K != E; ++K) {
      Value *Elt = B.CreateExtractElement(V, K);
      R = MinMax ? B.CreateBinaryIntrinsic(MinMax, R, Elt)
                 : B.CreateBinOp(Op, R, Elt);
    }
    // The start value of fadd and fmul, unless it is the identity.
    if (VecArg == 1) {
      auto *C = dyn_cast<ConstantFP>(II->getArgOperand(0));
      bool Identity =
          C && (Op == Instruction::FAdd ? C->isZero() : C->isExactlyValue(1.0));
      if (!Identity)
        R = B.CreateBinOp(Op, II->getArgOperand(0), R);
    }
    II->replaceAllUsesWith(R);
    II->eraseFromParent();
    Changed = true;
  }
  return Changed;
}

bool VUSinkFields::runOnFunction(Function &F) {
  bool Changed = expandReductions(F);
  SmallVector<Instruction *, 16> Fields;
  for (Instruction &I : instructions(F))
    if (isField(I))
      Fields.push_back(&I);
  // Later fields first: a splat of an extracted field sinks with it.
  for (Instruction *I : reverse(Fields)) {
    DenseMap<BasicBlock *, Instruction *> Copies;
    for (Use &U : make_early_inc_range(I->uses())) {
      auto *User = cast<Instruction>(U.getUser());
      BasicBlock *BB = User->getParent();
      if (BB == I->getParent() || isa<PHINode>(User))
        continue;
      Instruction *&C = Copies[BB];
      if (!C) {
        C = I->clone();
        C->setName(I->getName());
        C->insertBefore(BB->getFirstInsertionPt());
      }
      U.set(C);
      Changed = true;
    }
    if (I->use_empty())
      I->eraseFromParent();
  }
  return Changed;
}
