//===-------- LoopDataPrefetch.cpp - Loop Data Prefetching Pass -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a Loop Data Prefetching Pass.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/LoopDataPrefetch.h"
#include "ScalarOptions.h"
#include "llvm/InitializePasses.h"

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/CodeMetrics.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#define DEBUG_TYPE "loop-data-prefetch"

using namespace llvm;

STATISTIC(NumPrefetches, "Number of prefetches inserted");

namespace {

/// Loop prefetch implementation class.
class LoopDataPrefetch {
public:
  LoopDataPrefetch(const ScalarOptions &Opts, AssumptionCache *AC,
                   DominatorTree *DT, LoopInfo *LI, ScalarEvolution *SE,
                   const TargetTransformInfo *TTI,
                   OptimizationRemarkEmitter *ORE)
      : Opts(Opts), AC(AC), DT(DT), LI(LI), SE(SE), TTI(TTI), ORE(ORE) {}

  bool run();

private:
  bool runOnLoop(Loop *L, bool Versioned = false);
  bool isStreamCopy(Loop *L, StoreInst *SI);
  bool versionOnFootprint(Loop *L, uint64_t BytesPerIter, uint64_t MinBytes);

  /// Check if the stride of the accesses is large enough to
  /// warrant a prefetch.
  bool isStrideLargeEnough(const SCEVAddRecExpr *AR, unsigned TargetMinStride);

  unsigned getMinPrefetchStride(unsigned NumMemAccesses,
                                unsigned NumStridedMemAccesses,
                                unsigned NumPrefetches,
                                bool HasCall) {
    if (Opts.min_prefetch_stride)
      return *Opts.min_prefetch_stride;
    return TTI->getMinPrefetchStride(NumMemAccesses, NumStridedMemAccesses,
                                     NumPrefetches, HasCall);
  }

  unsigned getPrefetchDistance() {
    if (Opts.prefetch_distance)
      return *Opts.prefetch_distance;
    return TTI->getPrefetchDistance();
  }

  unsigned getMaxPrefetchIterationsAhead() {
    if (Opts.max_prefetch_iters_ahead)
      return *Opts.max_prefetch_iters_ahead;
    return TTI->getMaxPrefetchIterationsAhead();
  }

  bool doPrefetchWrites() {
    return valueOr(Opts.loop_prefetch_writes, TTI->enableWritePrefetching());
  }

  const ScalarOptions &Opts;
  AssumptionCache *AC;
  DominatorTree *DT;
  LoopInfo *LI;
  ScalarEvolution *SE;
  const TargetTransformInfo *TTI;
  OptimizationRemarkEmitter *ORE;
};

/// Legacy class for inserting loop data prefetches.
class LoopDataPrefetchLegacyPass : public FunctionPass {
public:
  static char ID; // Pass ID, replacement for typeid
  LoopDataPrefetchLegacyPass() : FunctionPass(ID) {
    initializeLoopDataPrefetchLegacyPassPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<AssumptionCacheTracker>();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addPreserved<DominatorTreeWrapperPass>();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addPreserved<LoopInfoWrapperPass>();
    AU.addRequiredID(LoopSimplifyID);
    AU.addPreservedID(LoopSimplifyID);
    AU.addRequired<OptimizationRemarkEmitterWrapperPass>();
    AU.addRequired<ScalarEvolutionWrapperPass>();
    AU.addPreserved<ScalarEvolutionWrapperPass>();
    AU.addRequired<TargetTransformInfoWrapperPass>();
  }

  bool runOnFunction(Function &F) override;
  };
}

char LoopDataPrefetchLegacyPass::ID = 0;
INITIALIZE_PASS_BEGIN(LoopDataPrefetchLegacyPass, "loop-data-prefetch",
                      "Loop Data Prefetch", false, false)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(TargetTransformInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopSimplify)
INITIALIZE_PASS_DEPENDENCY(OptimizationRemarkEmitterWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_END(LoopDataPrefetchLegacyPass, "loop-data-prefetch",
                    "Loop Data Prefetch", false, false)

FunctionPass *llvm::createLoopDataPrefetchPass() {
  return new LoopDataPrefetchLegacyPass();
}

bool LoopDataPrefetch::isStrideLargeEnough(const SCEVAddRecExpr *AR,
                                           unsigned TargetMinStride) {
  // No need to check if any stride goes.
  if (TargetMinStride <= 1)
    return true;

  const auto *ConstStride = dyn_cast<SCEVConstant>(AR->getStepRecurrence(*SE));
  // If MinStride is set, don't prefetch unless we can ensure that stride is
  // larger.
  if (!ConstStride)
    return false;

  unsigned AbsStride = std::abs(ConstStride->getAPInt().getSExtValue());
  return TargetMinStride <= AbsStride;
}

PreservedAnalyses LoopDataPrefetchPass::run(Function &F,
                                            FunctionAnalysisManager &AM) {
  DominatorTree *DT = &AM.getResult<DominatorTreeAnalysis>(F);
  LoopInfo *LI = &AM.getResult<LoopAnalysis>(F);
  ScalarEvolution *SE = &AM.getResult<ScalarEvolutionAnalysis>(F);
  AssumptionCache *AC = &AM.getResult<AssumptionAnalysis>(F);
  OptimizationRemarkEmitter *ORE =
      &AM.getResult<OptimizationRemarkEmitterAnalysis>(F);
  const TargetTransformInfo *TTI = &AM.getResult<TargetIRAnalysis>(F);

  const ScalarOptions &Opts =
      F.getContext().getLibraryOptions().get<ScalarOptions>();
  LoopDataPrefetch LDP(Opts, AC, DT, LI, SE, TTI, ORE);
  bool Changed = LDP.run();

  if (Changed) {
    PreservedAnalyses PA;
    PA.preserve<DominatorTreeAnalysis>();
    PA.preserve<LoopAnalysis>();
    return PA;
  }

  return PreservedAnalyses::all();
}

bool LoopDataPrefetchLegacyPass::runOnFunction(Function &F) {
  if (skipFunction(F))
    return false;

  DominatorTree *DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  LoopInfo *LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  ScalarEvolution *SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  AssumptionCache *AC =
      &getAnalysis<AssumptionCacheTracker>().getAssumptionCache(F);
  OptimizationRemarkEmitter *ORE =
      &getAnalysis<OptimizationRemarkEmitterWrapperPass>().getORE();
  const TargetTransformInfo *TTI =
      &getAnalysis<TargetTransformInfoWrapperPass>().getTTI(F);

  const ScalarOptions &Opts =
      F.getContext().getLibraryOptions().get<ScalarOptions>();
  LoopDataPrefetch LDP(Opts, AC, DT, LI, SE, TTI, ORE);
  return LDP.run();
}

/// Version \p L on its trip count: the loop continues in L if it accesses more
/// than \p MinBytes, \p BytesPerIter per iteration, and in a copy of it
/// otherwise. Returns false if the trip count can not be computed on entry.
bool LoopDataPrefetch::versionOnFootprint(Loop *L, uint64_t BytesPerIter,
                                          uint64_t MinBytes) {
  BasicBlock *Preheader = L->getLoopPreheader();
  BasicBlock *Exit = L->getUniqueExitBlock();
  BasicBlock *Exiting = L->getExitingBlock();
  if (!L->isInnermost() || !Preheader || !Exit || !Exiting)
    return false;
  const SCEV *BTC = SE->getBackedgeTakenCount(L);
  if (isa<SCEVCouldNotCompute>(BTC))
    return false;
  LLVMContext &Ctx = Preheader->getContext();
  Type *I64 = Type::getInt64Ty(Ctx);
  if (BTC->getType()->getScalarSizeInBits() > 64)
    return false;
  const SCEV *TripCount =
      SE->getAddExpr(SE->getZeroExtendExpr(BTC, I64), SE->getOne(I64));
  SCEVExpander Expander(*SE, "prefetch.check");
  if (!Expander.isSafeToExpand(TripCount))
    return false;

  // The check, in the old preheader; the loop gets a new one.
  BasicBlock *CheckBB = Preheader;
  BasicBlock *NewPreheader =
      SplitBlock(CheckBB, CheckBB->getTerminator(), DT, LI, nullptr,
                 L->getHeader()->getName() + ".ph");
  IRBuilder<> Builder(CheckBB->getTerminator());
  Value *Count =
      Expander.expandCodeFor(TripCount, I64, CheckBB->getTerminator());
  // TripCount * BytesPerIter > MinBytes, without overflow.
  Value *Big = Builder.CreateICmpUGT(
      Count, ConstantInt::get(I64, MinBytes / BytesPerIter), "prefetch.big");

  // The copy without prefetches.
  ValueToValueMapTy VMap;
  SmallVector<BasicBlock *, 8> Blocks;
  Loop *Plain = cloneLoopWithPreheader(NewPreheader, CheckBB, L, VMap,
                                       ".noprefetch", LI, DT, Blocks);
  remapInstructionsInBlocks(Blocks, VMap);
  Instruction *OldTerm = CheckBB->getTerminator();
  Builder.SetInsertPoint(OldTerm);
  Builder.CreateCondBr(Big, NewPreheader, Plain->getLoopPreheader());
  OldTerm->eraseFromParent();
  DT->changeImmediateDominator(Exit, CheckBB);

  // The exit is reached from both loops.
  auto *PlainExiting = cast<BasicBlock>(VMap[Exiting]);
  for (PHINode &PN : Exit->phis()) {
    Value *V = PN.getIncomingValueForBlock(Exiting);
    Value *Mapped = VMap.lookup(V);
    PN.addIncoming(Mapped ? Mapped : V, PlainExiting);
  }
  for (BasicBlock *BB : L->blocks())
    for (Instruction &I : *BB) {
      SmallVector<Use *, 4> Outside;
      for (Use &U : I.uses()) {
        auto *UI = cast<Instruction>(U.getUser());
        if (!L->contains(UI) && !Plain->contains(UI) &&
            !(isa<PHINode>(UI) && UI->getParent() == Exit))
          Outside.push_back(&U);
      }
      if (Outside.empty())
        continue;
      PHINode *PN = PHINode::Create(I.getType(), 2, I.getName() + ".pf",
                                    Exit->getFirstNonPHIIt());
      PN->addIncoming(&I, Exiting);
      PN->addIncoming(VMap[&I], PlainExiting);
      for (Use *U : Outside)
        U->set(PN);
    }
  formDedicatedExitBlocks(L, DT, LI, nullptr, false);
  formDedicatedExitBlocks(Plain, DT, LI, nullptr, false);
  SE->forgetLoop(L);
  return true;
}

bool LoopDataPrefetch::run() {
  // If PrefetchDistance is not set, don't run the pass.  This gives an
  // opportunity for targets to run this pass for selected subtargets only
  // (whose TTI sets PrefetchDistance and CacheLineSize).
  if (getPrefetchDistance() == 0 || TTI->getCacheLineSize() == 0) {
    LLVM_DEBUG(dbgs() << "Please set both PrefetchDistance and CacheLineSize "
                         "for loop data prefetch.\n");
    return false;
  }

  bool MadeChange = false;

  // Versioning adds loops: visit the ones there are now.
  SmallVector<Loop *, 16> Loops;
  for (Loop *I : *LI)
    for (Loop *L : depth_first(I))
      Loops.push_back(L);
  for (Loop *L : Loops)
    MadeChange |= runOnLoop(L);

  return MadeChange;
}

/// A record for a potential prefetch made during the initial scan of the
/// loop. This is used to let a single prefetch target multiple memory accesses.
struct Prefetch {
  /// The address formula for this prefetch as returned by ScalarEvolution.
  const SCEVAddRecExpr *LSCEVAddRec;
  /// The point of insertion for the prefetch instruction.
  Instruction *InsertPt = nullptr;
  /// True if targeting a write memory access.
  bool Writes = false;
  /// The (first seen) prefetched instruction.
  Instruction *MemI = nullptr;

  /// Constructor to create a new Prefetch for \p I.
  Prefetch(const SCEVAddRecExpr *L, Instruction *I) : LSCEVAddRec(L) {
    addInstruction(I);
  };

  /// Add the instruction \param I to this prefetch. If it's not the first
  /// one, 'InsertPt' and 'Writes' will be updated as required.
  /// \param PtrDiff the known constant address difference to the first added
  /// instruction.
  void addInstruction(Instruction *I, DominatorTree *DT = nullptr,
                      int64_t PtrDiff = 0) {
    if (!InsertPt) {
      MemI = I;
      InsertPt = I;
      Writes = isa<StoreInst>(I);
    } else {
      BasicBlock *PrefBB = InsertPt->getParent();
      BasicBlock *InsBB = I->getParent();
      if (PrefBB != InsBB) {
        BasicBlock *DomBB = DT->findNearestCommonDominator(PrefBB, InsBB);
        if (DomBB != PrefBB)
          InsertPt = DomBB->getTerminator();
      }

      if (isa<StoreInst>(I) && PtrDiff == 0)
        Writes = true;
    }
  }
};

/// Whether \p SI stores what the loop loaded from a stream with the same
/// step: a copy of one array into another.
bool LoopDataPrefetch::isStreamCopy(Loop *L, StoreInst *SI) {
  auto *LI = dyn_cast<LoadInst>(SI->getValueOperand());
  if (!LI || !L->contains(LI))
    return false;
  const auto *S =
      dyn_cast<SCEVAddRecExpr>(SE->getSCEV(SI->getPointerOperand()));
  const auto *Ld =
      dyn_cast<SCEVAddRecExpr>(SE->getSCEV(LI->getPointerOperand()));
  return S && Ld && S->getLoop() == L && Ld->getLoop() == L &&
         S->getStepRecurrence(*SE) == Ld->getStepRecurrence(*SE);
}

bool LoopDataPrefetch::runOnLoop(Loop *L, bool Versioned) {
  bool MadeChange = false;

  // Only prefetch in the inner-most loop
  if (!L->isInnermost())
    return MadeChange;

  SmallPtrSet<const Value *, 32> EphValues;
  CodeMetrics::collectEphemeralValues(L, AC, EphValues);

  // Calculate the number of iterations ahead to prefetch
  CodeMetrics Metrics;
  bool HasCall = false;
  for (const auto BB : L->blocks()) {
    // If the loop already has prefetches, then assume that the user knows
    // what they are doing and don't add any more.
    for (auto &I : *BB) {
      if (isa<CallInst>(&I) || isa<InvokeInst>(&I)) {
        if (const Function *F = cast<CallBase>(I).getCalledFunction()) {
          if (F->getIntrinsicID() == Intrinsic::prefetch)
            return MadeChange;
          if (TTI->isLoweredToCall(F))
            HasCall = true;
        } else { // indirect call.
          HasCall = true;
        }
      }
    }
    Metrics.analyzeBasicBlock(BB, *TTI, EphValues);
  }

  if (!Metrics.NumInsts.isValid())
    return MadeChange;

  unsigned LoopSize = Metrics.NumInsts.getValue();
  if (!LoopSize)
    LoopSize = 1;

  unsigned ItersAhead = Opts.prefetch_distance
                            ? std::max(1u, *Opts.prefetch_distance / LoopSize)
                            : TTI->getPrefetchIterationsAhead(L, *SE, LoopSize);

  if (ItersAhead > getMaxPrefetchIterationsAhead())
    return MadeChange;

  unsigned ConstantMaxTripCount = SE->getSmallConstantMaxTripCount(L);
  if (ConstantMaxTripCount && ConstantMaxTripCount < ItersAhead + 1)
    return MadeChange;

  unsigned NumMemAccesses = 0;
  unsigned NumStridedMemAccesses = 0;
  SmallVector<Prefetch, 16> Prefetches;
  // The strided stores that are not prefetched: they count to the bytes the
  // loop accesses.
  SmallVector<const SCEVAddRecExpr *, 8> OtherStores;
  for (const auto BB : L->blocks())
    for (auto &I : *BB) {
      Value *PtrValue;
      Instruction *MemI;

      if (LoadInst *LMemI = dyn_cast<LoadInst>(&I)) {
        MemI = LMemI;
        PtrValue = LMemI->getPointerOperand();
      } else if (StoreInst *SMemI = dyn_cast<StoreInst>(&I)) {
        if (!doPrefetchWrites() ||
            (!TTI->shouldPrefetchCopyStores() && isStreamCopy(L, SMemI))) {
          if (!L->isLoopInvariant(SMemI->getPointerOperand()))
            if (const auto *AR = dyn_cast<SCEVAddRecExpr>(
                    SE->getSCEV(SMemI->getPointerOperand())))
              OtherStores.push_back(AR);
          continue;
        }
        MemI = SMemI;
        PtrValue = SMemI->getPointerOperand();
      } else continue;

      unsigned PtrAddrSpace = PtrValue->getType()->getPointerAddressSpace();
      if (!TTI->shouldPrefetchAddressSpace(PtrAddrSpace))
        continue;
      NumMemAccesses++;
      if (L->isLoopInvariant(PtrValue))
        continue;

      const SCEV *LSCEV = SE->getSCEV(PtrValue);
      const SCEVAddRecExpr *LSCEVAddRec = dyn_cast<SCEVAddRecExpr>(LSCEV);
      if (!LSCEVAddRec)
        continue;
      NumStridedMemAccesses++;

      // We don't want to double prefetch individual cache lines. If this
      // access is known to be within one cache line of some other one that
      // has already been prefetched, then don't prefetch this one as well.
      bool DupPref = false;
      for (auto &Pref : Prefetches) {
        const SCEV *PtrDiff = SE->getMinusSCEV(LSCEVAddRec, Pref.LSCEVAddRec);
        if (const SCEVConstant *ConstPtrDiff =
            dyn_cast<SCEVConstant>(PtrDiff)) {
          int64_t PD = std::abs(ConstPtrDiff->getValue()->getSExtValue());
          if (PD < (int64_t) TTI->getCacheLineSize()) {
            Pref.addInstruction(MemI, DT, PD);
            DupPref = true;
            break;
          }
        }
      }
      if (!DupPref)
        Prefetches.push_back(Prefetch(LSCEVAddRec, MemI));
    }

  unsigned TargetMinStride =
    getMinPrefetchStride(NumMemAccesses, NumStridedMemAccesses,
                         Prefetches.size(), HasCall);

  LLVM_DEBUG(dbgs() << "Prefetching " << ItersAhead
             << " iterations ahead (loop size: " << LoopSize << ") in "
             << L->getHeader()->getParent()->getName() << ": " << *L);
  LLVM_DEBUG(dbgs() << "Loop has: "
             << NumMemAccesses << " memory accesses, "
             << NumStridedMemAccesses << " strided memory accesses, "
             << Prefetches.size() << " potential prefetch(es), "
             << "a minimum stride of " << TargetMinStride << ", "
             << (HasCall ? "calls" : "no calls") << ".\n");

  // The bytes an iteration accesses: the step of each group of accesses within
  // a cache line of each other.
  uint64_t BytesPerIter = 0;
  SmallVector<const SCEVAddRecExpr *, 16> Groups;
  for (const Prefetch &P : Prefetches)
    Groups.push_back(P.LSCEVAddRec);
  for (const SCEVAddRecExpr *AR : OtherStores)
    if (none_of(Groups, [&](const SCEVAddRecExpr *G) {
          const auto *D = dyn_cast<SCEVConstant>(SE->getMinusSCEV(AR, G));
          return D && std::abs(D->getValue()->getSExtValue()) <
                          int64_t(TTI->getCacheLineSize());
        }))
      Groups.push_back(AR);
  for (const SCEVAddRecExpr *AR : Groups)
    if (const auto *Step = dyn_cast<SCEVConstant>(AR->getStepRecurrence(*SE)))
      BytesPerIter += Step->getAPInt().abs().getLimitedValue();

  // Only the accesses whose stride is large enough to warrant a prefetch.
  llvm::erase_if(Prefetches, [&](const Prefetch &P) {
    return !isStrideLargeEnough(P.LSCEVAddRec, TargetMinStride);
  });
  if (Prefetches.empty())
    return MadeChange;

  // Prefetches that pay only if the loop accesses enough memory: decide on a
  // constant trip count, or version the loop on its trip count.
  uint64_t MinBytes =
      Versioned ? 0 : TTI->getPrefetchMinFootprint(L, *SE, Prefetches.size());
  if (MinBytes == UINT64_MAX)
    return MadeChange;
  if (MinBytes) {
    if (unsigned TripCount = SE->getSmallConstantTripCount(L)) {
      if (uint64_t(TripCount) * BytesPerIter <= MinBytes)
        return MadeChange;
    } else {
      if (!BytesPerIter || !versionOnFootprint(L, BytesPerIter, MinBytes))
        return MadeChange;
      return runOnLoop(L, /*Versioned=*/true) || true;
    }
  }

  for (auto &P : Prefetches) {

    BasicBlock *BB = P.InsertPt->getParent();
    SCEVExpander SCEVE(*SE, "prefaddr");
    const SCEV *NextLSCEV = SE->getAddExpr(
        P.LSCEVAddRec,
        SE->getMulExpr(SE->getConstant(P.LSCEVAddRec->getType(), ItersAhead),
                       P.LSCEVAddRec->getStepRecurrence(*SE)));
    if (!SCEVE.isSafeToExpand(NextLSCEV))
      continue;

    unsigned PtrAddrSpace = NextLSCEV->getType()->getPointerAddressSpace();
    Type *I8Ptr = PointerType::get(BB->getContext(), PtrAddrSpace);
    Value *PrefPtrValue = SCEVE.expandCodeFor(NextLSCEV, I8Ptr, P.InsertPt);

    IRBuilder<> Builder(P.InsertPt);
    Type *I32 = Type::getInt32Ty(BB->getContext());
    Builder.CreateIntrinsic(Intrinsic::prefetch, PrefPtrValue->getType(),
                            {PrefPtrValue, ConstantInt::get(I32, P.Writes),
                             ConstantInt::get(I32, 3),
                             ConstantInt::get(I32, 1)});
    ++NumPrefetches;
    LLVM_DEBUG(dbgs() << "  Access: "
               << *P.MemI->getOperand(isa<LoadInst>(P.MemI) ? 0 : 1)
               << ", SCEV: " << *P.LSCEVAddRec << "\n");
    ORE->emit([&]() {
        return OptimizationRemark(DEBUG_TYPE, "Prefetched", P.MemI)
          << "prefetched memory access";
      });

    MadeChange = true;
  }

  return MadeChange;
}
