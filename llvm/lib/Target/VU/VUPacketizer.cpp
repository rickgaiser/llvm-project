//===-- VUPacketizer.cpp - Pair upper and lower instructions --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Bundles an upper and a lower instruction into one micro instruction word.
// The schedulers have already ordered the instructions by cycle, so this pass
// packs neighbours in program order. Both halves of a word read their operands
// before either of them writes a result, so:
//  - a half can not read a result of the other half, except Q or P from a
//    WAITQ or WAITP (an operand latency of zero),
//  - a half can read a register that the other half writes: it sees the old
//    value,
//  - both halves must not write the same register, because the result of the
//    lower instruction would be discarded.
//
// A word stalls as a whole when one of its halves waits for an operand. The
// packetizer tracks the cycle in which each word issues and does not add an
// instruction to a word that would make it stall.
//
//===----------------------------------------------------------------------===//

#include "VU.h"
#include "VUSubtarget.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/CodeGen/DFAPacketizer.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/ScheduleDAG.h"
#include "llvm/InitializePasses.h"

using namespace llvm;

#define DEBUG_TYPE "vu-packetizer"

namespace {
class VUPacketizerList : public VLIWPacketizerList {
public:
  VUPacketizerList(MachineFunction &MF, MachineLoopInfo &MLI)
      : VLIWPacketizerList(MF, MLI, /*AA=*/nullptr) {}

  bool ignorePseudoInstruction(const MachineInstr &MI,
                               const MachineBasicBlock *MBB) override {
    return MI.isMetaInstruction();
  }

  // The end of the program and inline assembly get words of their own.
  bool isSoloInstruction(const MachineInstr &MI) override {
    return !MI.isMetaInstruction() && (MI.isPseudo() || MI.isInlineAsm());
  }

  bool isLegalToPacketizeTogether(SUnit *SUI, SUnit *SUJ) override;
  bool shouldAddToPacket(const MachineInstr &MI) override;
  MachineBasicBlock::iterator addToPacket(MachineInstr &MI) override;
  void endPacket(MachineBasicBlock *MBB,
                 MachineBasicBlock::iterator MI) override;

  void startRegion() {
    IssueCycle.clear();
    CurCycle = NextCycle = 0;
  }

private:
  /// Returns the first cycle in which \p MI can issue without a stall.
  unsigned getReadyCycle(const MachineInstr &MI);

  // The cycle in which each packetized instruction of the region issues.
  DenseMap<const MachineInstr *, unsigned> IssueCycle;
  // The cycle of the current word, and the first cycle of the next one.
  unsigned CurCycle = 0, NextCycle = 0;
};

class VUPacketizer : public MachineFunctionPass {
public:
  static char ID;
  VUPacketizer() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "VU Packetizer"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<MachineLoopInfoWrapperPass>();
    AU.addPreserved<MachineLoopInfoWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override;
};
} // namespace

char VUPacketizer::ID = 0;

INITIALIZE_PASS_BEGIN(VUPacketizer, DEBUG_TYPE, "VU Packetizer", false, false)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_END(VUPacketizer, DEBUG_TYPE, "VU Packetizer", false, false)

FunctionPass *llvm::createVUPacketizerPass() { return new VUPacketizer(); }

// SUJ is already in the packet and comes before SUI.
bool VUPacketizerList::isLegalToPacketizeTogether(SUnit *SUI, SUnit *SUJ) {
  for (const SDep &Dep : SUJ->Succs) {
    if (Dep.getSUnit() != SUI)
      continue;
    switch (Dep.getKind()) {
    case SDep::Anti:
      break;
    case SDep::Data:
      if (Dep.getLatency() != 0)
        return false;
      break;
    case SDep::Output:
    case SDep::Order:
      return false;
    }
  }
  return true;
}

unsigned VUPacketizerList::getReadyCycle(const MachineInstr &MI) {
  unsigned Ready = 0;
  SUnit *SU = MIToSUnit[const_cast<MachineInstr *>(&MI)];
  for (const SDep &Pred : SU->Preds) {
    auto It = IssueCycle.find(Pred.getSUnit()->getInstr());
    if (It != IssueCycle.end())
      Ready = std::max(Ready, It->second + Pred.getLatency());
  }
  return Ready;
}

bool VUPacketizerList::shouldAddToPacket(const MachineInstr &MI) {
  return getReadyCycle(MI) <= CurCycle;
}

MachineBasicBlock::iterator VUPacketizerList::addToPacket(MachineInstr &MI) {
  if (CurrentPacketMIs.empty())
    CurCycle = std::max(NextCycle, getReadyCycle(MI));
  IssueCycle[&MI] = CurCycle;
  return VLIWPacketizerList::addToPacket(MI);
}

void VUPacketizerList::endPacket(MachineBasicBlock *MBB,
                                 MachineBasicBlock::iterator MI) {
  if (!CurrentPacketMIs.empty())
    NextCycle = CurCycle + 1;
  VLIWPacketizerList::endPacket(MBB, MI);
}

bool VUPacketizer::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;

  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  MachineLoopInfo &MLI = getAnalysis<MachineLoopInfoWrapperPass>().getLI();
  VUPacketizerList Packetizer(MF, MLI);

  for (MachineBasicBlock &MBB : MF) {
    auto Begin = MBB.begin(), End = MBB.end();
    while (Begin != End) {
      // Packetize each scheduling region together with the boundary that
      // ends it, so that a branch can share its word with an upper
      // instruction.
      MachineBasicBlock::iterator RB = Begin;
      while (RB != End && TII->isSchedulingBoundary(*RB, &MBB, MF))
        ++RB;
      MachineBasicBlock::iterator RE = RB;
      while (RE != End && !TII->isSchedulingBoundary(*RE, &MBB, MF))
        ++RE;
      if (RE != End)
        ++RE;
      if (RB != End) {
        Packetizer.startRegion();
        Packetizer.PacketizeMIs(&MBB, RB, RE);
      }
      Begin = RE;
    }
  }
  return true;
}
