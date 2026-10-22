//===- MipsR5900IssuePass.cpp - Reorder for R5900 dual issue --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The R5900 issues two instructions together only if they are in the same
// aligned 8-byte block (see MipsR5900IssueModel.h). The post-RA scheduler does
// not know the addresses of the instructions, and pseudo expansion and the
// delay slot filler run after it, so this pass schedules each basic block
// again once the addresses are known.
//
// The blocks are filled as for a VLIW: the second slot of a block gets an
// instruction that issues together with the first, or a nop if there is none.
// Then each nop that does not make the basic block faster is removed. The new
// order is kept only if the simulated basic block is faster.
//
//===----------------------------------------------------------------------===//

#include "Mips.h"
#include "MipsInstrInfo.h"
#include "MipsR5900IssueModel.h"
#include "MipsSubtarget.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolutionAliasAnalysis.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineOptimizationRemarkEmitter.h"
#include "llvm/CodeGen/ScheduleDAGInstrs.h"
#include "llvm/CodeGen/TargetSchedule.h"
#include "llvm/Support/CommandLine.h"
#include <optional>

using namespace llvm;

#define DEBUG_TYPE "mips-r5900-issue"

STATISTIC(NumBlocksReordered, "Number of basic blocks reordered");
STATISTIC(NumNopsInserted, "Number of nops inserted");

static cl::opt<bool>
    DisableR5900Issue("disable-mips-r5900-issue", cl::init(false), cl::Hidden,
                      cl::desc("Do not reorder instructions for the dual "
                               "issue of aligned 8-byte blocks on the R5900"));

// Removing the nops simulates the basic block for each nop, for each nop that
// is removed.
static cl::opt<unsigned> R5900IssueNopLimit(
    "mips-r5900-issue-nop-limit", cl::init(1024), cl::Hidden,
    cl::desc("Do not insert nops for the R5900 dual issue in basic blocks of "
             "more instructions"));

namespace {

/// Builds the dependence graph of a region; the pass schedules it.
class R5900IssueDAG : public ScheduleDAGInstrs {
public:
  R5900IssueDAG(MachineFunction &MF)
      : ScheduleDAGInstrs(MF, nullptr, /*RemoveKillFlags=*/true) {}

  void schedule() override {
    // The delay slot filler invalidates liveness, as it moves instructions
    // between blocks, but buildSchedGraph reads the live-ins of the
    // successors for the registers that are live at the end of a region. They
    // only give the latencies to the end of the region, which stays in place,
    // so a stale list does not affect correctness.
    MachineFunctionProperties &Props = MF.getProperties();
    bool TracksLiveness = Props.hasTracksLiveness();
    Props.setTracksLiveness();
    buildSchedGraph(nullptr);
    if (!TracksLiveness)
      Props.resetTracksLiveness();
  }

  const DbgValueVector &getDbgValues() const { return DbgValues; }
};

class MipsR5900Issue : public MachineFunctionPass {
public:
  static char ID;

  explicit MipsR5900Issue(bool KeepSCEVAA = false)
      : MachineFunctionPass(ID), KeepSCEVAA(KeepSCEVAA) {}

  StringRef getPassName() const override { return "Mips R5900 Dual Issue"; }

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoVRegs();
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineLoopInfoWrapperPass>();
    AU.addRequired<MachineOptimizationRemarkEmitterPass>();
    // The alias analysis results of the schedulers refer to the SCEV based
    // alias analysis added before instruction selection, but only use it if
    // available, which does not keep it alive. Requiring it here does.
    if (KeepSCEVAA)
      AU.addRequired<SCEVAAWrapperPass>();
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  bool KeepSCEVAA;
  const MipsInstrInfo *TII = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  const MachineLoopInfo *MLI = nullptr;
  TargetSchedModel SchedModel;
  bool OptForSize = false;
  /// Nops are only inserted in loops that are not too large.
  bool AllowNops = false;
  std::unique_ptr<R5900IssueDAG> DAG;
  /// The nops inserted in the current basic block.
  SmallVector<MachineInstr *, 8> Nops;

  bool isBoundary(const MachineInstr &MI) const;
  unsigned simulate(const MachineBasicBlock &MBB, unsigned StartSlot) const;
  void scheduleRegion(MachineBasicBlock &MBB, MachineBasicBlock::iterator Begin,
                      MachineBasicBlock::iterator End,
                      MipsR5900IssueModel &Model);
  bool scheduleBlock(MachineBasicBlock &MBB, unsigned StartSlot,
                     MachineOptimizationRemarkEmitter &ORE);
};

} // end anonymous namespace

char MipsR5900Issue::ID = 0;

INITIALIZE_PASS(MipsR5900Issue, DEBUG_TYPE, "Mips R5900 Dual Issue", false,
                false)

FunctionPass *llvm::createMipsR5900IssuePass(bool KeepSCEVAA) {
  return new MipsR5900Issue(KeepSCEVAA);
}

/// Returns the size of \p MI in bytes, or nothing if it is not known.
static std::optional<unsigned> getSize(const MachineInstr &MI) {
  if (MI.isMetaInstruction())
    return 0;
  if (MI.isInlineAsm() || !MI.getDesc().getSize())
    return std::nullopt;
  return MI.getDesc().getSize();
}

bool MipsR5900Issue::isBoundary(const MachineInstr &MI) const {
  const MachineBasicBlock *MBB = MI.getParent();
  return MI.isBundled() || MI.isCall() || MI.hasDelaySlot() ||
         MI.isInlineAsm() || MI.hasUnmodeledSideEffects() ||
         TII->isSchedulingBoundary(MI, MBB, *MBB->getParent());
}

/// Returns the simulated cycles of \p MBB, whose first instruction is in slot
/// \p StartSlot of its 8-byte block. For a loop of a single basic block,
/// returns the cycles of an iteration after the first ones.
unsigned MipsR5900Issue::simulate(const MachineBasicBlock &MBB,
                                  unsigned StartSlot) const {
  MipsR5900IssueModel Model(SchedModel, *TRI);
  unsigned NumIters = MBB.isSuccessor(&MBB) ? 3 : 1;
  unsigned Start = 0;
  for (unsigned Iter = 0; Iter != NumIters; ++Iter) {
    Model.startBlock(StartSlot);
    Start = Model.getNextBlockCycle();
    for (const MachineInstr &MI : MBB.instrs())
      Model.issue(MI);
  }
  return Model.getNextBlockCycle() - Start;
}

void MipsR5900Issue::scheduleRegion(MachineBasicBlock &MBB,
                                    MachineBasicBlock::iterator Begin,
                                    MachineBasicBlock::iterator End,
                                    MipsR5900IssueModel &Model) {
  DAG->enterRegion(&MBB, Begin, End, std::distance(Begin, End));
  DAG->schedule();

  SmallVector<unsigned, 32> NumPredsLeft;
  SmallVector<SUnit *, 16> Ready;
  for (SUnit &SU : DAG->SUnits) {
    NumPredsLeft.push_back(SU.NumPredsLeft);
    if (!SU.NumPredsLeft)
      Ready.push_back(&SU);
  }

  // List schedule into the slots of the 8-byte blocks. Pick the instruction
  // that issues first, then the one with the longest path to the end of the
  // region, then the first one.
  SmallVector<MachineInstr *, 32> Order;
  while (!Ready.empty()) {
    SUnit **Best = nullptr;
    unsigned BestCycle = 0;
    for (SUnit *&SU : Ready) {
      unsigned Cycle = Model.getIssueCycle(*SU->getInstr());
      if (!Best || Cycle < BestCycle ||
          (Cycle == BestCycle && (SU->getHeight() > (*Best)->getHeight() ||
                                  (SU->getHeight() == (*Best)->getHeight() &&
                                   SU->NodeNum < (*Best)->NodeNum)))) {
        Best = &SU;
        BestCycle = Cycle;
      }
    }
    SUnit *SU = *Best;

    // Leave the second slot of a block empty if nothing issues together with
    // the first instruction.
    if (AllowNops && Model.getSlot() == 1 &&
        !Model.wouldPair(*SU->getInstr())) {
      MachineInstr *Nop =
          BuildMI(*MBB.getParent(), DebugLoc(), TII->get(Mips::SLL), Mips::R0)
              .addReg(Mips::R0)
              .addImm(0);
      Model.issue(*Nop);
      Order.push_back(Nop);
      Nops.push_back(Nop);
      continue;
    }

    Model.issue(*SU->getInstr());
    Order.push_back(SU->getInstr());
    *Best = Ready.back();
    Ready.pop_back();
    for (const SDep &Succ : SU->Succs) {
      SUnit *SuccSU = Succ.getSUnit();
      if (Succ.isWeak() || SuccSU->isBoundaryNode())
        continue;
      if (!--NumPredsLeft[SuccSU->NodeNum])
        Ready.push_back(SuccSU);
    }
  }
  assert(Order.size() -
                 count_if(Order,
                          [](MachineInstr *MI) { return !MI->getParent(); }) ==
             DAG->SUnits.size() &&
         "Not all instructions were scheduled");

  for (MachineInstr *MI : Order) {
    if (MI->getParent())
      MBB.splice(End, &MBB, MI);
    else
      MBB.insert(End, MI);
  }
  // Put each debug value back after the instruction it followed.
  for (const auto &[DbgValue, OrigPrev] : reverse(DAG->getDbgValues()))
    MBB.splice(std::next(OrigPrev->getIterator()), &MBB, DbgValue);

  DAG->exitRegion();
}

bool MipsR5900Issue::scheduleBlock(MachineBasicBlock &MBB, unsigned StartSlot,
                                   MachineOptimizationRemarkEmitter &ORE) {
  unsigned OldCycles = simulate(MBB, StartSlot);
  SmallVector<MachineInstr *, 32> OldOrder;
  for (MachineInstr &MI : MBB)
    OldOrder.push_back(&MI);
  Nops.clear();
  AllowNops =
      !OptForSize && MLI->getLoopFor(&MBB) && MBB.size() <= R5900IssueNopLimit;

  MipsR5900IssueModel Model(SchedModel, *TRI);
  Model.startBlock(StartSlot);
  DAG->startBlock(&MBB);
  for (MachineBasicBlock::iterator I = MBB.begin(), E = MBB.end(); I != E;) {
    MachineBasicBlock::iterator RegionEnd = I;
    while (RegionEnd != E && !isBoundary(*RegionEnd))
      ++RegionEnd;
    if (RegionEnd != I)
      scheduleRegion(MBB, I, RegionEnd, Model);
    if (RegionEnd == E)
      break;
    // Issue the boundary and the instructions bundled with it.
    for (auto BI = RegionEnd.getInstrIterator(),
              BE = std::next(RegionEnd).getInstrIterator();
         BI != BE; ++BI)
      Model.issue(*BI);
    I = std::next(RegionEnd);
  }
  DAG->finishBlock();

  // Remove the nops that do not make the basic block faster, each time the
  // one that leaves the fewest cycles. Removing a nop moves the instructions
  // after it to the other slot, so also try to remove two nops together, which
  // keeps the slots after the second one.
  unsigned NewCycles = simulate(MBB, StartSlot);
  auto CyclesWithout = [&](ArrayRef<MachineInstr *> Remove) {
    SmallVector<MachineBasicBlock::iterator, 2> Next;
    for (MachineInstr *Nop : Remove) {
      Next.push_back(std::next(Nop->getIterator()));
      MBB.remove(Nop);
    }
    unsigned Cycles = simulate(MBB, StartSlot);
    for (unsigned I = Remove.size(); I--;)
      MBB.insert(Next[I], Remove[I]);
    return Cycles;
  };
  for (;;) {
    unsigned BestCycles = NewCycles + 1, BestIdx = 0, BestNum = 0;
    for (unsigned I = 0, E = Nops.size(); I != E; ++I) {
      for (unsigned Num = 1; Num <= 2 && I + Num <= E; ++Num) {
        unsigned Cycles = CyclesWithout(ArrayRef(Nops).slice(I, Num));
        if (Cycles < BestCycles || (Cycles == BestCycles && Num > BestNum)) {
          BestCycles = Cycles;
          BestIdx = I;
          BestNum = Num;
        }
      }
    }
    if (!BestNum)
      break;
    for (MachineInstr *Nop : ArrayRef(Nops).slice(BestIdx, BestNum))
      Nop->eraseFromParent();
    Nops.erase(Nops.begin() + BestIdx, Nops.begin() + BestIdx + BestNum);
    NewCycles = BestCycles;
  }

  bool Reordered = NewCycles < OldCycles;
  if (!Reordered) {
    for (MachineInstr *Nop : Nops)
      Nop->eraseFromParent();
    for (MachineInstr *MI : OldOrder)
      MBB.splice(MBB.end(), &MBB, MI);
    NewCycles = OldCycles;
  } else {
    ++NumBlocksReordered;
    NumNopsInserted += Nops.size();
  }

  ORE.emit([&]() {
    DebugLoc DL = MBB.empty() ? DebugLoc() : MBB.begin()->getDebugLoc();
    return MachineOptimizationRemarkAnalysis(DEBUG_TYPE, "DualIssue", DL, &MBB)
           << MBB.getParent()->getName() << " %bb."
           << ore::NV("Block", MBB.getNumber()) << ", slot "
           << ore::NV("StartSlot", StartSlot) << ": "
           << ore::NV("OldCycles", OldCycles) << " -> "
           << ore::NV("NewCycles", NewCycles) << " cycles, "
           << ore::NV("Nops", Reordered ? Nops.size() : 0) << " nops";
  });
  return Reordered;
}

bool MipsR5900Issue::runOnMachineFunction(MachineFunction &MF) {
  const MipsSubtarget &STI = MF.getSubtarget<MipsSubtarget>();
  if (!STI.isR5900() || DisableR5900Issue || skipFunction(MF.getFunction()))
    return false;

  TII = STI.getInstrInfo();
  TRI = STI.getRegisterInfo();
  SchedModel.init(&STI);
  MLI = &getAnalysis<MachineLoopInfoWrapperPass>().getLI();
  OptForSize = MF.getFunction().hasOptSize();
  DAG = std::make_unique<R5900IssueDAG>(MF);
  auto &ORE = getAnalysis<MachineOptimizationRemarkEmitterPass>().getORE();

  // The offset of each basic block in its 8-byte block, if it is known.
  std::optional<unsigned> Offset;
  if (MF.getAlignment() >= Align(8))
    Offset = 0;
  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    if (MBB.getAlignment() >= Align(8))
      Offset = 0;
    if (Offset && all_of(MBB.instrs(), [](const MachineInstr &MI) {
          return getSize(MI).has_value();
        }))
      Changed |= scheduleBlock(MBB, *Offset / 4, ORE);
    for (const MachineInstr &MI : MBB.instrs()) {
      std::optional<unsigned> Size = getSize(MI);
      if (!Offset || !Size)
        Offset = std::nullopt;
      else
        Offset = (*Offset + *Size) % 8;
    }
  }
  DAG.reset();
  return Changed;
}
