//===-- VUDelaySlotFiller.cpp - VU branch delay slots and hazards ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Every branch has a delay slot. The words have been formed already, so the
// branch is moved into the word before it, and the lower instruction of that
// word, if any, into the delay slot with the upper instruction of the branch
// word:
//   maddw.xyzw vf2, vf7, vf0w  iaddiu vi5, vi6, 1    maddw.xyzw vf2, ...  ibne
//   nop                        ibne vi4, vi3, L  ->  nop iaddiu nop nop
// The words keep their places, so the Q hazards found earlier stay valid.
// Otherwise the delay slot is filled with a NOP.
//
// A conditional branch must not use a VI register written by the word right
// before it, so a NOP is inserted in that case as well.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUMCTargetDesc.h"
#include "VU.h"
#include "VUSubtarget.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineInstrBundle.h"

using namespace llvm;

#define DEBUG_TYPE "vu-delay-slot-filler"

namespace {
class VUDelaySlotFiller : public MachineFunctionPass {
public:
  static char ID;
  VUDelaySlotFiller() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "VU Delay Slot Filler"; }
  bool runOnMachineFunction(MachineFunction &MF) override;
};
} // namespace

char VUDelaySlotFiller::ID = 0;

INITIALIZE_PASS(VUDelaySlotFiller, DEBUG_TYPE, "VU Delay Slot Filler", false,
                false)

FunctionPass *llvm::createVUDelaySlotFillerPass() {
  return new VUDelaySlotFiller();
}

/// Returns the branch or call in the micro instruction word \p MI, which may
/// be a bundle, or nullptr.
static MachineInstr *getBranch(MachineInstr &MI) {
  if (!MI.isBundle())
    return MI.isBranch() || MI.isCall() ? &MI : nullptr;
  for (auto I = std::next(MI.getIterator());
       I != MI.getParent()->instr_end() && I->isInsideBundle(); ++I)
    if (I->isBranch() || I->isCall())
      return &*I;
  return nullptr;
}

/// Returns true if the word \p Prev writes a VI register that \p Branch
/// reads.
static bool writesBranchCondition(const MachineInstr &Prev,
                                  const MachineInstr &Branch,
                                  const TargetRegisterInfo &TRI) {
  for (const MachineOperand &MO : Branch.uses())
    if (MO.isReg() && MO.getReg() && VU::VIRegClass.contains(MO.getReg()) &&
        Prev.modifiesRegister(MO.getReg(), &TRI))
      return true;
  return false;
}

/// Returns the instructions of the word \p W.
static SmallVector<MachineInstr *, 2> getInstrs(MachineInstr &W) {
  if (!W.isBundle())
    return {&W};
  SmallVector<MachineInstr *, 2> Instrs;
  for (auto I = std::next(W.getIterator());
       I != W.getParent()->instr_end() && I->isInsideBundle(); ++I)
    if (!I->isMetaInstruction())
      Instrs.push_back(&*I);
  return Instrs;
}

/// Returns the word before \p I in its block, skipping meta instructions.
static MachineInstr *getPrevWord(MachineBasicBlock::iterator I) {
  MachineBasicBlock &MBB = *I->getParent();
  while (I != MBB.begin()) {
    --I;
    if (!I->isMetaInstruction())
      return &*I;
  }
  return nullptr;
}

/// Returns true if \p MI can be executed one word later, after a branch.
static bool canDelay(const MachineInstr &MI) {
  // A division or a wait would change the words in which Q and P are valid.
  return !MI.isBranch() && !MI.isCall() && !MI.isTerminator() &&
         !MI.isPseudo() && !MI.isInlineAsm() && !MI.hasUnmodeledSideEffects() &&
         MI.getOpcode() != VU::WAITQ && MI.getOpcode() != VU::WAITP &&
         !MI.modifiesRegister(VU::Q, /*TRI=*/nullptr) &&
         !MI.modifiesRegister(VU::P, /*TRI=*/nullptr);
}

/// Returns the number of cycles that readers in the successors of the block
/// may stall more when \p Lower moves from the word before the branch into
/// the delay slot. The readers are then two words closer: the branch word is
/// gone and \p Lower is one word later.
static unsigned getExtraStall(const MachineInstr &Lower,
                              const TargetInstrInfo &TII,
                              const InstrItineraryData *Itin) {
  int Extra = 0;
  for (MachineBasicBlock *Succ : Lower.getParent()->successors()) {
    // The word of the reader, counted from the start of the successor.
    int Word = 0;
    for (MachineInstr &W : *Succ) {
      if (W.isMetaInstruction())
        continue;
      for (MachineInstr *Use : getInstrs(W))
        for (const MachineOperand &MO : Use->all_uses())
          for (const MachineOperand &Def : Lower.all_defs()) {
            if (MO.getReg() != Def.getReg())
              continue;
            int Latency = TII.getOperandLatency(Itin, Lower, Def.getOperandNo(),
                                                *Use, MO.getOperandNo())
                              .value_or(1);
            Extra = std::max(Extra, std::max(0, Latency - 1 - Word) -
                                        std::max(0, Latency - 3 - Word));
          }
      if (++Word == 8)
        break;
    }
  }
  return Extra;
}

/// Moves \p Branch into the word before its word \p BranchWord, and the
/// lower instruction of that word into the delay slot. Returns the delay slot
/// word, or nullptr if this is not possible.
static MachineInstr *fillDelaySlot(MachineInstr &BranchWord,
                                   MachineInstr &Branch,
                                   const TargetInstrInfo &TII,
                                   const TargetRegisterInfo &TRI) {
  MachineBasicBlock &MBB = *BranchWord.getParent();
  MachineInstr *PrevWord = getPrevWord(BranchWord);
  if (!PrevWord || PrevWord->isTerminator() || PrevWord->isInlineAsm() ||
      (!PrevWord->isBundle() && PrevWord->isPseudo()))
    return nullptr;
  // The branch must not read a VI register written by the word before its
  // new word, which is unknown at the start of the block.
  if (Branch.isConditionalBranch()) {
    MachineInstr *PrevPrev = getPrevWord(*PrevWord);
    if (!PrevPrev || writesBranchCondition(*PrevPrev, Branch, TRI))
      return nullptr;
  }

  MachineInstr *Lower = nullptr, *Upper = nullptr;
  for (MachineInstr *MI : getInstrs(*PrevWord)) {
    // The branch now reads its registers in this word, before the writes.
    if (writesBranchCondition(*MI, Branch, TRI))
      return nullptr;
    if (VU::isLowerInstr(MI->getDesc()))
      Lower = MI;
  }
  for (MachineInstr *MI : getInstrs(BranchWord))
    if (MI != &Branch)
      Upper = MI;
  if (Lower) {
    // Saving the branch word is worth one more stall cycle.
    if (!canDelay(*Lower) ||
        getExtraStall(*Lower, TII,
                      MBB.getParent()->getSubtarget().getInstrItineraryData()) >
            1)
      return nullptr;
    // Lower moves into the word of Upper, which no longer sees its results.
    if (Upper)
      for (const MachineOperand &MO : Lower->all_defs())
        if (Upper->readsRegister(MO.getReg(), &TRI) ||
            Upper->modifiesRegister(MO.getReg(), &TRI))
          return nullptr;
  }

  // The upper instruction of the previous word and the branch, then the
  // delay slot.
  SmallVector<MachineInstr *, 2> First, Second;
  for (MachineInstr *MI : getInstrs(*PrevWord))
    if (MI != Lower)
      First.push_back(MI);
  First.push_back(&Branch);
  if (Upper)
    Second.push_back(Upper);
  if (Lower)
    Second.push_back(Lower);

  auto InsertPt = std::next(MachineBasicBlock::iterator(BranchWord));
  for (MachineInstr *Word : {PrevWord, &BranchWord}) {
    if (!Word->isBundle())
      continue;
    for (MachineInstr *MI = Word->getNextNode();
         MI && MI->isBundledWithPred();) {
      MachineInstr *Next = MI->getNextNode();
      MI->unbundleFromPred();
      MI = Next;
    }
    Word->eraseFromParent();
  }
  auto buildWord = [&](ArrayRef<MachineInstr *> Instrs) {
    for (MachineInstr *MI : Instrs)
      MBB.splice(InsertPt, &MBB, MI);
    if (Instrs.size() == 1)
      return Instrs.front();
    finalizeBundle(MBB, Instrs.front()->getIterator(),
                   std::next(Instrs.back()->getIterator()));
    return Instrs.front()->getPrevNode();
  };
  buildWord(First);
  DebugLoc DL = Branch.getDebugLoc();
  if (Second.empty())
    return BuildMI(MBB, InsertPt, DL, TII.get(VU::DSNOP));
  Second.push_back(BuildMI(MBB, InsertPt, DL, TII.get(VU::DELAY_SLOT)));
  return buildWord(Second);
}

bool VUDelaySlotFiller::runOnMachineFunction(MachineFunction &MF) {
  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
  const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();
  bool Changed = false;

  // Iterate over words: bundles and the instructions outside of them.
  for (MachineBasicBlock &MBB : MF) {
    for (auto I = MBB.begin(), E = MBB.end(); I != E; ++I) {
      MachineInstr *Branch = getBranch(*I);
      if (!Branch)
        continue;

      if (!Branch->isCall()) {
        if (MachineInstr *Slot = fillDelaySlot(*I, *Branch, TII, TRI)) {
          I = Slot->getIterator();
          Changed = true;
          continue;
        }
      }

      if (Branch->isConditionalBranch()) {
        // Find the previous word in this block. At the start of a block it
        // is unknown, so be conservative.
        auto Prev = I;
        while (Prev != MBB.begin() && std::prev(Prev)->isMetaInstruction())
          --Prev;
        if (Prev == MBB.begin() ||
            writesBranchCondition(*std::prev(Prev), *Branch, TRI)) {
          BuildMI(MBB, I, Branch->getDebugLoc(), TII.get(VU::NOP));
          Changed = true;
        }
      }

      BuildMI(MBB, std::next(I), Branch->getDebugLoc(), TII.get(VU::DSNOP));
      ++I;
      Changed = true;
    }
  }
  return Changed;
}
