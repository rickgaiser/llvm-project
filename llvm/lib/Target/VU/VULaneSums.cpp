//===-- VULaneSums.cpp - Add fields of vectors in loops with broadcasts ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A sum of fields of vectors, like the dot product (t.y + t.z) + t.x, is
// selected as a chain through ACC with vf0, in the w field:
//   MULAy.w acc, vf0, t; MADDAz.w acc, vf0, t; MADDx.w d, vf0, t
// It can also be computed in the field of its first term, adding the other
// terms as broadcasts:
//   ADDz.y d, t, t.z; ADDx.y d, d, t.x
// which takes one FMAC instruction less, but a few cycles more. In a loop,
// which the pipeliner overlaps, the instructions count; elsewhere the
// latency. So in loops, the sums are rewritten as vector additions of
// broadcasts, in the same order, which gives the same result.
//
//===----------------------------------------------------------------------===//

#include "VU.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"

using namespace llvm;
using namespace llvm::PatternMatch;

#define DEBUG_TYPE "vu-lane-sums"

namespace {
class VULaneSums : public FunctionPass {
public:
  static char ID;
  VULaneSums() : FunctionPass(ID) {}

  StringRef getPassName() const override { return "VU Lane Sums"; }
  bool runOnFunction(Function &F) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LoopInfoWrapperPass>();
    AU.setPreservesCFG();
  }
};

/// A term of a sum: field Lane of the vector Vec, or a scalar if Vec is
/// null.
struct LaneTerm {
  Value *Vec;
  unsigned Lane;
  Value *Scalar;
  bool Neg;
};
} // namespace

char VULaneSums::ID = 0;

INITIALIZE_PASS_BEGIN(VULaneSums, DEBUG_TYPE, "VU Lane Sums", false, false)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_END(VULaneSums, DEBUG_TYPE, "VU Lane Sums", false, false)

FunctionPass *llvm::createVULaneSumsPass() { return new VULaneSums(); }

static bool isSum(Value *V) {
  auto *I = dyn_cast<Instruction>(V);
  return I && I->getType()->isFloatTy() && I->hasOneUse() &&
         (I->getOpcode() == Instruction::FAdd ||
          I->getOpcode() == Instruction::FSub);
}

static LaneTerm getTerm(Value *V, bool Neg) {
  Value *Vec;
  uint64_t Lane;
  if (match(V, m_ExtractElt(m_Value(Vec), m_ConstantInt(Lane))) &&
      cast<FixedVectorType>(Vec->getType())->getNumElements() == 4)
    return {Vec, unsigned(Lane), nullptr, Neg};
  return {nullptr, 0, V, Neg};
}

/// Collects the terms of a left-linear chain of FADD and FSUB, in evaluation
/// order.
static void collectTerms(Instruction *I, SmallVectorImpl<LaneTerm> &Terms) {
  Value *L = I->getOperand(0), *R = I->getOperand(1);
  if (I->getOpcode() == Instruction::FAdd && !isSum(L) && isSum(R))
    std::swap(L, R);
  if (isSum(L))
    collectTerms(cast<Instruction>(L), Terms);
  else
    Terms.push_back(getTerm(L, /*Neg=*/false));
  Terms.push_back(getTerm(R, I->getOpcode() == Instruction::FSub));
}

bool VULaneSums::runOnFunction(Function &F) {
  if (skipFunction(F))
    return false;
  LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  SmallVector<Instruction *, 8> Roots;
  for (BasicBlock &BB : F) {
    if (!LI.getLoopFor(&BB))
      continue;
    for (Instruction &I : BB)
      if ((I.getOpcode() == Instruction::FAdd ||
           I.getOpcode() == Instruction::FSub) &&
          I.getType()->isFloatTy() && !(I.hasOneUse() && isSum(I.user_back())))
        Roots.push_back(&I);
  }

  bool Changed = false;
  for (Instruction *Root : Roots) {
    SmallVector<LaneTerm, 4> Terms;
    collectTerms(Root, Terms);
    // Only sums of fields; a field other than x would otherwise be moved.
    if (Terms[0].Neg ||
        none_of(Terms, [](const LaneTerm &T) { return T.Vec && T.Lane != 0; }))
      continue;

    IRBuilder<> B(Root);
    Type *VecTy = FixedVectorType::get(B.getFloatTy(), 4);
    auto getVector = [&](const LaneTerm &T) -> std::pair<Value *, unsigned> {
      if (T.Vec)
        return {T.Vec, T.Lane};
      return {
          B.CreateInsertElement(PoisonValue::get(VecTy), T.Scalar, uint64_t(0)),
          0};
    };
    auto [Sum, Lane] = getVector(Terms[0]);
    for (const LaneTerm &T : ArrayRef(Terms).drop_front()) {
      auto [Vec, K] = getVector(T);
      Value *Splat = B.CreateShuffleVector(Vec, SmallVector<int, 4>(4, K));
      Sum = T.Neg ? B.CreateFSub(Sum, Splat) : B.CreateFAdd(Sum, Splat);
    }
    Value *Result = B.CreateExtractElement(Sum, uint64_t(Lane));
    Result->takeName(Root);
    // The chain has no other users; the dead instructions go later.
    Root->replaceAllUsesWith(Result);
    Changed = true;
  }
  return Changed;
}
