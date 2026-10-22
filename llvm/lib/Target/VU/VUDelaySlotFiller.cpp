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
// Otherwise the delay slot is filled with a NOP. VUHazards runs afterwards.
//
// A conditional branch must not use a VI register written by the word right
// before it, so a NOP is inserted in that case as well.
//
// In a single block loop, any lower instruction that may run after the branch
// can fill the delay slot, which can also remove its word or a stall. The
// layout with the fewest cycles per iteration is used.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUMCTargetDesc.h"
#include "VU.h"
#include "VUSubtarget.h"
#include "VUWords.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineInstrBundle.h"
#include "llvm/CodeGen/TargetInstrInfo.h"

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
      for (MachineInstr *Use : VU::getWordInstrs(W))
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
  for (MachineInstr *MI : VU::getWordInstrs(*PrevWord)) {
    // The branch now reads its registers in this word, before the writes.
    if (writesBranchCondition(*MI, Branch, TRI))
      return nullptr;
    if (VU::isLowerInstr(MI->getDesc()))
      Lower = MI;
  }
  for (MachineInstr *MI : VU::getWordInstrs(BranchWord))
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
  for (MachineInstr *MI : VU::getWordInstrs(*PrevWord))
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

/// Returns true if \p Prev writes a VI register that \p Branch reads.
static bool writesBranchCondition(const VU::Word &Prev,
                                  const MachineInstr &Branch,
                                  const TargetRegisterInfo &TRI) {
  for (MachineInstr *MI : {Prev.Upper, Prev.Lower})
    if (MI && writesBranchCondition(*MI, Branch, TRI))
      return true;
  return false;
}

/// Returns true if \p L, the lower instruction of word \p K, may run after
/// the words after it and the upper instruction of its word. \p SlotUpper
/// runs in the same word as \p L.
static bool canMoveToEnd(ArrayRef<VU::Word> Words, unsigned K,
                         const MachineInstr &L, const MachineInstr *SlotUpper,
                         const TargetInstrInfo &TII,
                         const TargetRegisterInfo &TRI) {
  if (!canDelay(L))
    return false;
  // X runs after L, unless it is in the word of L: the upper instruction of
  // that word reads before L writes, and SlotUpper writes after L reads.
  auto Conflicts = [&](const MachineInstr &X) {
    bool Before = &X == Words[K].Upper, After = &X == SlotUpper;
    for (const MachineOperand &MO : L.all_defs())
      if ((!Before && X.readsRegister(MO.getReg(), &TRI)) ||
          X.modifiesRegister(MO.getReg(), &TRI))
        return true;
    for (const MachineOperand &MO : L.all_uses())
      if (MO.getReg() && !After && X.modifiesRegister(MO.getReg(), &TRI))
        return true;
    if ((L.mayStore() && (X.mayLoad() || X.mayStore())) ||
        (L.mayLoad() && X.mayStore()))
      return !TII.areMemAccessesTriviallyDisjoint(L, X);
    return X.hasUnmodeledSideEffects();
  };
  if (Words[K].Upper && Conflicts(*Words[K].Upper))
    return false;
  for (unsigned I = K + 1, E = Words.size(); I != E; ++I)
    for (MachineInstr *X : {Words[I].Upper, Words[I].Lower})
      if (X && Conflicts(*X))
        return false;
  return true;
}

/// Adds a NOP word before the branch if the word before it writes its
/// condition, and the delay slot.
static void finishLayout(SmallVectorImpl<VU::Word> &Words, MachineInstr &Branch,
                         VU::Word Slot, const TargetRegisterInfo &TRI) {
  unsigned B = Words.size() - 1;
  if (Branch.isConditionalBranch() &&
      (B == 0 || writesBranchCondition(Words[B - 1], Branch, TRI)))
    Words.insert(Words.begin() + B, VU::Word());
  Slot.DelaySlot = true;
  Words.push_back(Slot);
}

/// Fills the delay slot of the single block loop \p MBB, whose last word
/// holds its only branch. Returns false if the block does not have this form.
static bool fillLoopDelaySlot(MachineBasicBlock &MBB,
                              const TargetInstrInfo &TII,
                              const TargetRegisterInfo &TRI) {
  SmallVector<VU::Word, 32> Words;
  MachineInstr *Branch = nullptr;
  for (MachineInstr &W : MBB) {
    if (W.isMetaInstruction())
      continue;
    if (Branch)
      return false;
    VU::Word New;
    for (MachineInstr *MI : VU::getWordInstrs(W)) {
      if (MI->isBranch() || MI->isCall()) {
        if (MI->isCall() || &W != &*std::prev(MBB.end()))
          return false;
        Branch = MI;
      }
      if (MI->isPseudo() && MI->getOpcode() != VU::NOP)
        return false;
      (VU::isLowerInstr(MI->getDesc()) ? New.Lower : New.Upper) = MI;
    }
    Words.push_back(New);
  }
  if (!Branch || Words.size() < 2)
    return false;

  const InstrItineraryData *Itin =
      MBB.getParent()->getSubtarget().getInstrItineraryData();
  VU::Word BranchWord = Words.back();
  ArrayRef<VU::Word> Body = ArrayRef(Words).drop_back();

  // The delay slot holds a NOP, or the upper instruction of the branch word
  // together with a lower instruction from an earlier word, when the branch
  // can move into the word before it.
  SmallVector<VU::Word, 32> Best(Body);
  Best.push_back(BranchWord);
  finishLayout(Best, *Branch, VU::Word(), TRI);
  int BestCycles = VU::getLoopCycles(Best, TII, Itin, TRI);
  auto Consider = [&](SmallVectorImpl<VU::Word> &New, const char *What) {
    int Cycles = VU::getLoopCycles(New, TII, Itin, TRI);
    LLVM_DEBUG(dbgs() << "Delay slot " << What << ": " << Cycles
                      << " cycles, before " << BestCycles << "\n");
    if (Cycles < BestCycles ||
        (Cycles == BestCycles && New.size() < Best.size())) {
      Best = std::move(New);
      BestCycles = Cycles;
    }
  };
  // Only the upper instruction of the branch word in the delay slot.
  if (!Body.back().Lower && !writesBranchCondition(Body.back(), *Branch, TRI)) {
    SmallVector<VU::Word, 32> New(Body);
    New.back().Lower = Branch;
    VU::Word Slot;
    Slot.Upper = BranchWord.Upper;
    finishLayout(New, *Branch, Slot, TRI);
    Consider(New, "upper");
  }
  for (unsigned K = 0, E = Body.size(); K != E; ++K)
    for (bool MoveBranch : {true, false}) {
      MachineInstr *L = Body[K].Lower;
      MachineInstr *SlotUpper = MoveBranch ? BranchWord.Upper : nullptr;
      if (!L || !canMoveToEnd(Words, K, *L, SlotUpper, TII, TRI))
        continue;
      // The branch moves into the word before it if that word has no lower
      // instruction and does not write the condition, which the branch then
      // reads in the same word. The upper instruction of the branch word goes
      // into the delay slot.
      SmallVector<VU::Word, 32> New(Body);
      New[K].Lower = nullptr;
      VU::Word Slot;
      Slot.Lower = L;
      if (MoveBranch) {
        if (New.back().Lower || writesBranchCondition(New.back(), *Branch, TRI))
          continue;
        New.back().Lower = Branch;
        Slot.Upper = BranchWord.Upper;
      }
      if (!New[K].Upper && !New[K].Lower)
        New.erase(New.begin() + K);
      if (!MoveBranch)
        New.push_back(BranchWord);
      finishLayout(New, *Branch, Slot, TRI);
      Consider(New, TII.getName(L->getOpcode()).data());
    }

  VU::rebuildWords(MBB, Best, TII, Branch->getDebugLoc());
  return true;
}

bool VUDelaySlotFiller::runOnMachineFunction(MachineFunction &MF) {
  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
  const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();
  bool Changed = false;

  // Iterate over words: bundles and the instructions outside of them.
  for (MachineBasicBlock &MBB : MF) {
    if (MBB.isSuccessor(&MBB) && fillLoopDelaySlot(MBB, TII, TRI)) {
      Changed = true;
      continue;
    }
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
