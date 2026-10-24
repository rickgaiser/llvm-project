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
// A loop that only has to wait because of a division before it, such as one
// in the prologue of a pipelined loop, waits once on the way in instead of in
// every iteration: a WAITQ in a block of its own on a fall-through edge, or
// before the branch of a predecessor that jumps to the loop.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUMCTargetDesc.h"
#include "VU.h"
#include "VUSubtarget.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
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

/// Returns true if \p MBB needs a WAITQ when \p Need words are still needed
/// at its start, and sets \p Need to the words needed at its end.
static bool stepBlock(MachineBasicBlock &MBB, unsigned &Need) {
  bool Waits = false;
  for (MachineInstr &MI : MBB)
    Waits |= stepWord(getWord(MI), Need);
  return Waits;
}

namespace {
/// Where the WAITQ of an edge into a loop goes.
enum class EdgeWait { BeforeBranch, NewBlock, None };
} // namespace

/// Returns the word of the branch of \p Pred to \p MBB, or end().
static MachineBasicBlock::iterator getBranchTo(MachineBasicBlock &Pred,
                                               MachineBasicBlock &MBB) {
  for (auto I = Pred.begin(), E = Pred.end(); I != E; ++I)
    for (auto J = I.getInstrIterator(), JE = std::next(I).getInstrIterator();
         J != JE; ++J)
      if (J->isBranch() && any_of(J->operands(), [&](const MachineOperand &MO) {
            return MO.isMBB() && MO.getMBB() == &MBB;
          }))
        return I;
  return Pred.end();
}

/// Returns where a WAITQ on the edge from \p Pred to \p MBB can go: before
/// the branch to it (not if a division is in the delay slot after it), or
/// in a block of its own on the fall-through edge.
static EdgeWait getEdgeWait(MachineBasicBlock &Pred, MachineBasicBlock &MBB) {
  MachineBasicBlock::iterator Br = getBranchTo(Pred, MBB);
  if (Br == Pred.end())
    return Pred.getNextNode() == &MBB ? EdgeWait::NewBlock : EdgeWait::None;
  for (auto I = Br.getInstrIterator(), E = Pred.instr_end(); I != E; ++I)
    if (getQLatency(*I))
      return EdgeWait::None;
  return EdgeWait::BeforeBranch;
}

/// Inserts a WAITQ word on the edge from \p Pred to \p MBB.
static void waitOnEdge(MachineBasicBlock &Pred, MachineBasicBlock &MBB,
                       const TargetInstrInfo &TII) {
  DebugLoc DL;
  if (getEdgeWait(Pred, MBB) == EdgeWait::BeforeBranch) {
    // The WAITQ also delays the other edge of a conditional branch, once.
    BuildMI(Pred, getBranchTo(Pred, MBB), DL, TII.get(VU::WAITQ));
    return;
  }
  MachineFunction &MF = *MBB.getParent();
  MachineBasicBlock *Wait = MF.CreateMachineBasicBlock(MBB.getBasicBlock());
  MF.insert(MBB.getIterator(), Wait);
  for (const MachineBasicBlock::RegisterMaskPair &LI : MBB.liveins())
    Wait->addLiveIn(LI);
  BuildMI(*Wait, Wait->end(), DL, TII.get(VU::WAITQ));
  Pred.replaceSuccessor(&MBB, Wait);
  Wait->addSuccessor(&MBB);
}

bool VUHazards::runOnMachineFunction(MachineFunction &MF) {
  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();

  // Words still needed at the start of each block, over all predecessors.
  // A loop in WaitOnEntry waits on the edges into it, so only its own
  // back edge counts.
  DenseMap<const MachineBasicBlock *, unsigned> NeedIn;
  SmallPtrSet<MachineBasicBlock *, 4> WaitOnEntry;
  auto Propagate = [&] {
    NeedIn.clear();
    bool Iterate = true;
    while (Iterate) {
      Iterate = false;
      for (MachineBasicBlock &MBB : MF) {
        unsigned Need = NeedIn.lookup(&MBB);
        // A WAITQ is inserted where stepWord returns true.
        stepBlock(MBB, Need);
        for (MachineBasicBlock *Succ : MBB.successors()) {
          if (Succ != &MBB && WaitOnEntry.count(Succ))
            continue;
          unsigned &In = NeedIn[Succ];
          if (Need > In) {
            In = Need;
            Iterate = true;
          }
        }
      }
    }
  };
  Propagate();
  for (bool Found = true; Found;) {
    Found = false;
    for (MachineBasicBlock &MBB : MF) {
      if (!MBB.isSuccessor(&MBB) || WaitOnEntry.count(&MBB))
        continue;
      unsigned Need = NeedIn.lookup(&MBB);
      if (!stepBlock(MBB, Need))
        continue;
      // The words needed when only the loop's own division is waited for.
      unsigned Self = 0, Prev;
      do {
        Prev = Self;
        Need = Self;
        stepBlock(MBB, Need);
        Self = std::max(Self, Need);
      } while (Self != Prev);
      Need = Self;
      if (stepBlock(MBB, Need) ||
          any_of(MBB.predecessors(), [&](MachineBasicBlock *Pred) {
            return Pred != &MBB && getEdgeWait(*Pred, MBB) == EdgeWait::None;
          }))
        continue;
      WaitOnEntry.insert(&MBB);
      Found = true;
    }
    if (Found)
      Propagate();
  }

  bool Changed = false;
  for (MachineBasicBlock *Loop : WaitOnEntry) {
    MachineBasicBlock &MBB = *Loop;
    SmallVector<MachineBasicBlock *, 2> Preds;
    for (MachineBasicBlock *Pred : MBB.predecessors()) {
      unsigned Need = NeedIn.lookup(Pred);
      stepBlock(*Pred, Need);
      if (Pred != &MBB && Need > NeedIn.lookup(&MBB))
        Preds.push_back(Pred);
    }
    for (MachineBasicBlock *Pred : Preds)
      waitOnEdge(*Pred, MBB, TII);
    Changed |= !Preds.empty();
  }

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
