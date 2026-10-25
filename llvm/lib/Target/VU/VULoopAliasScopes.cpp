//===-- VULoopAliasScopes.cpp - Disjoint loads and stores of a loop -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A compute program reads its items and writes its results into the same
// buffer, behind them: x[n + 1 + 3 g] = f(x[16 g ...]). The pipeliner then
// keeps the loads of the next iteration after the stores of this one, unless
// it knows that no iteration stores to where another loads.
//
// For each loop, the bytes that a load or a store accesses over all
// iterations are [first, last + size] of its address, an add recurrence of
// the loop or invariant. When every load of the loop and every store of it
// through the same base are disjoint that way, the loads and the stores get
// an alias scope each, noalias to the other.
//
// The bounds involve the trip count, typically a quotient of the item count:
// 256 (n /u 16) <= 16 n + 16 is not known to ScalarEvolution. The difference
// of the bounds is lowered to what it is at least, with c (X /u c) <= X and
// X /u c >= 0, before it is tested. Addresses are assumed not to wrap: the
// accesses are in data memory.
//
//===----------------------------------------------------------------------===//

#include "VU.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Operator.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

#define DEBUG_TYPE "vu-loop-alias-scopes"

STATISTIC(NumLoops, "Number of bases of a loop with disjoint loads and stores");

static cl::opt<bool> Enable("vu-enable-loop-alias-scopes", cl::Hidden,
                            cl::init(true),
                            cl::desc("Alias scopes for the disjoint loads and "
                                     "stores of a loop"));

namespace {
class VULoopAliasScopes : public FunctionPass {
public:
  static char ID;
  VULoopAliasScopes() : FunctionPass(ID) {}

  StringRef getPassName() const override { return "VU Loop Alias Scopes"; }
  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<ScalarEvolutionWrapperPass>();
    AU.setPreservesCFG();
  }

private:
  ScalarEvolution *SE = nullptr;
  bool processLoop(Loop &L, Function &F);
};

/// The bytes that an access covers over all iterations of a loop: its first
/// and last address, and its size.
struct Range {
  const SCEV *Lo, *Hi;
  uint64_t Size;
};
} // namespace

char VULoopAliasScopes::ID = 0;

INITIALIZE_PASS_BEGIN(VULoopAliasScopes, DEBUG_TYPE, "VU Loop Alias Scopes",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_END(VULoopAliasScopes, DEBUG_TYPE, "VU Loop Alias Scopes",
                    false, false)

FunctionPass *llvm::createVULoopAliasScopesPass() {
  return new VULoopAliasScopes();
}

namespace {
/// An address without the address space casts in it, which the VU's
/// memories do not change: (gep (addrspacecast p), 4) is p + 4.
class StripCasts : public SCEVRewriteVisitor<StripCasts> {
public:
  using SCEVRewriteVisitor::SCEVRewriteVisitor;
  const SCEV *visitUnknown(const SCEVUnknown *Expr) {
    if (auto *ASC = dyn_cast<AddrSpaceCastOperator>(Expr->getValue()))
      return visit(SE.getSCEV(ASC->getOperand(0)));
    return Expr;
  }
};
} // namespace

/// \p S without the extensions and truncations around a value of type \p Ty.
static const SCEV *stripExtensions(const SCEV *S) {
  while (auto *C = dyn_cast<SCEVCastExpr>(S))
    S = C->getOperand();
  return S;
}

/// The terms of \p S, an add or a single term.
static SmallVector<SCEVUse, 4> getTerms(const SCEV *S) {
  SmallVector<SCEVUse, 4> Terms;
  if (auto *Add = dyn_cast<SCEVAddExpr>(S))
    Terms.append(Add->operands().begin(), Add->operands().end());
  else
    Terms.push_back(S);
  return Terms;
}

/// \p Op as k * Y, with k = 1 if it has no constant factor.
static std::pair<APInt, const SCEV *> splitFactor(const SCEV *Op) {
  if (auto *M = dyn_cast<SCEVMulExpr>(Op); M && M->getNumOperands() == 2)
    if (auto *C = dyn_cast<SCEVConstant>(M->getOperand(0)))
      return {C->getAPInt(), M->getOperand(1)};
  return {APInt(Op->getType()->getScalarSizeInBits(), 1), Op};
}

/// A value that \p D is at least: k (X /u c) is at least (k / c) X for k < 0
/// and c | k, and at least 0 for k > 0; then k X is at least 0 for k > 0,
/// where X is unsigned as the dividend of such a quotient. Null if a term
/// cannot be bound.
static const SCEV *lowerBound(const SCEV *D, ScalarEvolution &SE) {
  Type *Ty = D->getType();
  SmallVector<SCEVUse, 4> Ops;
  SmallPtrSet<const SCEV *, 4> Unsigned;
  for (const SCEV *Op : getTerms(D)) {
    auto [K, Y] = splitFactor(Op);
    auto *Div = dyn_cast<SCEVUDivExpr>(stripExtensions(Y));
    auto *C = Div ? dyn_cast<SCEVConstant>(Div->getRHS()) : nullptr;
    if (!C || C->getAPInt().isZero()) {
      Ops.push_back(Op);
      continue;
    }
    const SCEV *X =
        SE.getTruncateOrZeroExtend(stripExtensions(Div->getLHS()), Ty);
    Unsigned.insert(X);
    if (!K.isNegative())
      continue; // at least 0
    APInt Den = C->getAPInt().zextOrTrunc(K.getBitWidth());
    if (Den.isZero() || K.srem(Den) != 0)
      return nullptr;
    Ops.push_back(SE.getMulExpr(SE.getConstant(K.sdiv(Den)), X));
  }
  if (Ops.empty())
    return SE.getZero(Ty);
  SmallVector<SCEVUse, 4> Rest;
  for (const SCEV *Op : getTerms(SE.getAddExpr(Ops))) {
    auto [K, Y] = splitFactor(Op);
    if (!K.isNegative() && Unsigned.contains(Y))
      continue; // at least 0
    Rest.push_back(Op);
  }
  if (Rest.empty())
    return SE.getZero(Ty);
  return SE.getAddExpr(Rest);
}

bool VULoopAliasScopes::runOnFunction(Function &F) {
  if (!Enable || skipFunction(F))
    return false;
  SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  bool Changed = false;
  for (Loop *L : LI.getLoopsInPreorder())
    Changed |= processLoop(*L, F);
  return Changed;
}

bool VULoopAliasScopes::processLoop(Loop &L, Function &F) {
  const SCEV *BTC = SE->getBackedgeTakenCount(&L);
  if (isa<SCEVCouldNotCompute>(BTC)) {
    LLVM_DEBUG(dbgs() << "VU: no trip count for " << L);
    return false;
  }
  const DataLayout &DL = F.getDataLayout();

  // The range of the address of an access over the iterations.
  auto GetRange = [&](Instruction &I, Value *Ptr,
                      Type *Ty) -> std::optional<Range> {
    const SCEV *S = StripCasts(*SE).visit(SE->getSCEV(Ptr));
    Range R{S, S, DL.getTypeStoreSize(Ty).getFixedValue()};
    if (auto *AR = dyn_cast<SCEVAddRecExpr>(S)) {
      if (AR->getLoop() != &L || !AR->isAffine())
        return std::nullopt;
      auto *Step = dyn_cast<SCEVConstant>(AR->getStepRecurrence(*SE));
      if (!Step)
        return std::nullopt;
      Type *IdxTy = DL.getIndexType(AR->getType());
      const SCEV *Last =
          AR->evaluateAtIteration(SE->getTruncateOrZeroExtend(BTC, IdxTy), *SE);
      R.Lo = AR->getStart();
      R.Hi = Last;
      if (Step->getAPInt().isNegative())
        std::swap(R.Lo, R.Hi);
    } else if (!SE->isLoopInvariant(S, &L)) {
      return std::nullopt;
    }
    return R;
  };

  // The accesses with a range, by the base of their address. The others
  // are not tagged: they make no claim.
  struct Group {
    SmallVector<std::pair<Instruction *, Range>, 16> Loads, Stores;
  };
  MapVector<const SCEV *, Group> Groups;
  for (BasicBlock *BB : L.blocks())
    for (Instruction &I : *BB) {
      Value *Ptr;
      Type *Ty;
      bool IsLoad = false;
      if (auto *LdI = dyn_cast<LoadInst>(&I); LdI && LdI->isSimple()) {
        Ptr = LdI->getPointerOperand();
        Ty = LdI->getType();
        IsLoad = true;
      } else if (auto *St = dyn_cast<StoreInst>(&I); St && St->isSimple()) {
        Ptr = St->getPointerOperand();
        Ty = St->getValueOperand()->getType();
      } else {
        continue;
      }
      auto R = GetRange(I, Ptr, Ty);
      if (!R) {
        LLVM_DEBUG(dbgs() << "VU: no range for " << I << "\n");
        continue;
      }
      Group &G = Groups[SE->getPointerBase(R->Lo)];
      (IsLoad ? G.Loads : G.Stores).push_back({&I, *R});
    }

  // A ends before B starts: B.Lo - (A.Hi + A.Size) is at least 0.
  auto Before = [&](const Range &A, const Range &B) {
    const SCEV *D = SE->getMinusSCEV(
        B.Lo, SE->getAddExpr(A.Hi, SE->getConstant(A.Hi->getType(), A.Size)));
    if (isa<SCEVCouldNotCompute>(D))
      return false;
    const SCEV *Bound = lowerBound(D, *SE);
    LLVM_DEBUG(dbgs() << "VU: gap " << *D << ", at least "
                      << (Bound ? *Bound : *D) << "\n");
    return Bound && SE->isKnownNonNegative(Bound);
  };
  auto Tag = [](Instruction *I, MDNode *Scope, MDNode *NoAlias) {
    I->setMetadata(LLVMContext::MD_alias_scope,
                   MDNode::concatenate(
                       I->getMetadata(LLVMContext::MD_alias_scope), Scope));
    I->setMetadata(
        LLVMContext::MD_noalias,
        MDNode::concatenate(I->getMetadata(LLVMContext::MD_noalias), NoAlias));
  };
  MDBuilder MDB(F.getContext());
  bool Changed = false;
  for (auto &[Base, G] : Groups) {
    if (G.Loads.empty() || G.Stores.empty())
      continue;
    bool Disjoint = all_of(G.Loads, [&](auto &Ld) {
      return all_of(G.Stores, [&](auto &St) {
        if (Before(Ld.second, St.second) || Before(St.second, Ld.second))
          return true;
        LLVM_DEBUG(dbgs() << "VU: " << *Ld.first << " and " << *St.first
                          << " may overlap\n");
        return false;
      });
    });
    if (!Disjoint)
      continue;
    // A pair of scopes for each base: the claims are only between the
    // loads and the stores checked here.
    MDNode *Domain = MDB.createAnonymousAliasScopeDomain(F.getName());
    MDNode *In = MDNode::get(
        F.getContext(), {MDB.createAnonymousAliasScope(Domain, "loop.in")});
    MDNode *Out = MDNode::get(
        F.getContext(), {MDB.createAnonymousAliasScope(Domain, "loop.out")});
    for (auto &[LdI, R] : G.Loads)
      Tag(LdI, In, Out);
    for (auto &[St, R] : G.Stores)
      Tag(St, Out, In);
    ++NumLoops;
    Changed = true;
  }
  return Changed;
}
