//===-- VUHazards.cpp - Wait for Q where it is read too early -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The VU does not stall when Q is read before a DIV, SQRT or RSQRT has
// finished; the read returns the old value. A read is safe once the latency
// (7 or 13 cycles) has passed. A word takes at least one cycle, so a read is
// safe if it is at least that many words after the division on every path,
// including around loops. Otherwise a WAITQ is inserted: in the word of the
// reader when its lower half is free (the upper half of a WAITQ word sees the
// new Q), or in a word of its own before it.
//
// This runs last, on micro instruction words. A read in a delay slot waits in
// a word before the branch, since no word may come between the branch and its
// delay slot.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUMCTargetDesc.h"
#include "VU.h"
#include "VUSubtarget.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineInstrBundle.h"

using namespace llvm;

#define DEBUG_TYPE "vu-hazards"

namespace {
class VUHazards : public MachineFunctionPass {
public:
  static char ID;
  VUHazards() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "VU Hazards"; }
  bool runOnMachineFunction(MachineFunction &MF) override;
};
} // namespace

char VUHazards::ID = 0;

INITIALIZE_PASS(VUHazards, DEBUG_TYPE, "VU Hazards", false, false)

FunctionPass *llvm::createVUHazardsPass() { return new VUHazards(); }

/// Returns the latency of the Q result of \p MI in words, or 0.
static unsigned getQLatency(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
  case VU::DIV:
  case VU::SQRT:
    return 7;
  case VU::RSQRT:
    return 13;
  default:
    return 0;
  }
}

namespace {
/// The instructions of a micro instruction word.
struct Word {
  SmallVector<MachineInstr *, 2> Instrs;
  bool HasLower = false;
};
} // namespace

static Word getWord(MachineInstr &MI) {
  Word W;
  auto Add = [&](MachineInstr &I) {
    if (I.isMetaInstruction())
      return;
    W.Instrs.push_back(&I);
    W.HasLower |= VU::isLowerInstr(I.getDesc());
  };
  if (!MI.isBundle()) {
    Add(MI);
    return W;
  }
  for (auto I = std::next(MI.getIterator());
       I != MI.getParent()->instr_end() && I->isInsideBundle(); ++I)
    Add(*I);
  return W;
}

static bool readsQ(const Word &W) {
  return any_of(W.Instrs, [](const MachineInstr *MI) {
    return MI->getOpcode() != VU::WAITQ && MI->readsRegister(VU::Q, nullptr);
  });
}

/// Steps over one word. \p Need is the number of words, from the start of
/// this word, before Q may be read. Returns true if the word must wait; Need
/// is then updated as if it contains a WAITQ.
static bool stepWord(const Word &W, unsigned &Need) {
  bool Wait = readsQ(W) && Need > 0;
  bool Waits = Wait || any_of(W.Instrs, [](const MachineInstr *MI) {
                 return MI->getOpcode() == VU::WAITQ;
               });
  unsigned Latency = 0;
  for (const MachineInstr *MI : W.Instrs)
    Latency = std::max(Latency, getQLatency(*MI));
  if (Latency)
    Need = Latency - 1;
  else if (Waits)
    Need = 0;
  else if (Need)
    --Need;
  return Wait;
}

bool VUHazards::runOnMachineFunction(MachineFunction &MF) {
  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();

  // Words still needed at the start of each block, over all predecessors.
  DenseMap<const MachineBasicBlock *, unsigned> NeedIn;
  bool Iterate = true;
  while (Iterate) {
    Iterate = false;
    for (MachineBasicBlock &MBB : MF) {
      unsigned Need = NeedIn.lookup(&MBB);
      for (MachineInstr &MI : MBB) {
        // A WAITQ is inserted where stepWord returns true.
        stepWord(getWord(MI), Need);
      }
      for (MachineBasicBlock *Succ : MBB.successors()) {
        unsigned &In = NeedIn[Succ];
        if (Need > In) {
          In = Need;
          Iterate = true;
        }
      }
    }
  }

  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    unsigned Need = NeedIn.lookup(&MBB);
    for (auto I = MBB.begin(), E = MBB.end(); I != E; ++I) {
      Word W = getWord(*I);
      if (!stepWord(W, Need))
        continue;
      DebugLoc DL = W.Instrs.front()->getDebugLoc();
      bool DelaySlot = any_of(W.Instrs, [](const MachineInstr *MI) {
        return MI->getOpcode() == VU::DELAY_SLOT;
      });
      if (DelaySlot && W.HasLower) {
        auto BranchWord = std::prev(I);
        while (BranchWord->isMetaInstruction())
          --BranchWord;
        BuildMI(MBB, BranchWord, DL, TII.get(VU::WAITQ));
        Changed = true;
        continue;
      }
      if (!W.HasLower && I->isBundle()) {
        // Share the word of the reader, which is a bundle already: an upper
        // instruction and a marker such as DELAY_SLOT. It is bundled again
        // with the WAITQ first.
        MachineBasicBlock::instr_iterator B = std::next(I->getIterator()),
                                          E = B;
        while (E != MBB.instr_end() && E->isBundledWithPred())
          (E++)->unbundleFromPred();
        I->eraseFromParent();
        MachineInstr *Wait = BuildMI(MBB, B, DL, TII.get(VU::WAITQ));
        finalizeBundle(MBB, Wait->getIterator(), E);
        I = MachineBasicBlock::iterator(&*std::prev(Wait->getIterator()));
        Changed = true;
        continue;
      }
      MachineInstr *Wait = BuildMI(MBB, I, DL, TII.get(VU::WAITQ));
      if (!W.HasLower) {
        // Share the word of the reader.
        finalizeBundle(MBB, Wait->getIterator(), std::next(I->getIterator()));
        I = MachineBasicBlock::iterator(&*std::prev(Wait->getIterator()));
      }
      Changed = true;
    }
  }
  return Changed;
}
