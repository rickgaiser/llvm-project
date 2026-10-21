//===-- VUDelaySlotFiller.cpp - VU branch delay slots and hazards ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Every branch has a delay slot, which is filled with a NOP. A conditional
// branch must not use a VI register written by the instruction right before
// it, so a NOP is inserted in that case as well.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUMCTargetDesc.h"
#include "VU.h"
#include "VUSubtarget.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

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
static const MachineInstr *getBranch(const MachineInstr &MI) {
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

bool VUDelaySlotFiller::runOnMachineFunction(MachineFunction &MF) {
  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
  const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();
  bool Changed = false;

  // Iterate over words: bundles and the instructions outside of them.
  for (MachineBasicBlock &MBB : MF) {
    for (auto I = MBB.begin(), E = MBB.end(); I != E; ++I) {
      const MachineInstr *Branch = getBranch(*I);
      if (!Branch)
        continue;

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
