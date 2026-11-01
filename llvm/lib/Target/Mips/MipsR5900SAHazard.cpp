//===- MipsR5900SAHazard.cpp - R5900 SA register hazard -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The three instructions before an MTSAB, MTSAH or MTSA must not read the SA
// register (QFSRV, MFSA): EE Core Instruction Set Manual, MTSAB. This pass
// inserts nops before such a write of SA when one of the three instructions
// before it, also at the end of a predecessor block, reads SA. It runs last
// of the passes that reorder instructions (after MipsR5900IssuePass).
//
// MipsR5900SARedundant, before register allocation, deletes the MTSABs and
// MTSAHs that set SA to the value it already has: the lowering gives each
// QFSRV its own (MipsSETargetLowering::lowerLOAD).
//
//===----------------------------------------------------------------------===//

#include "Mips.h"
#include "MipsInstrInfo.h"
#include "MipsSubtarget.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

using namespace llvm;

#define DEBUG_TYPE "mips-r5900-sa-hazard"

STATISTIC(NumNops, "Number of nops inserted for the R5900 SA hazard");
STATISTIC(NumRedundant, "Number of redundant R5900 SA writes deleted");

namespace {

constexpr unsigned HazardDistance = 3;

bool readsSA(const MachineInstr &MI) {
  return MI.getOpcode() == Mips::QFSRV || MI.getOpcode() == Mips::MFSA;
}

bool writesSA(const MachineInstr &MI) {
  return MI.getOpcode() == Mips::MTSAB || MI.getOpcode() == Mips::MTSAH ||
         MI.getOpcode() == Mips::MTSA;
}

/// Whether an instruction is emitted: not a debug value, label or the like.
bool isEmitted(const MachineInstr &MI) {
  return !MI.isMetaInstruction() && !MI.isPosition() && !MI.isCFIInstruction();
}

class MipsR5900SAHazard : public MachineFunctionPass {
public:
  static char ID;
  MipsR5900SAHazard() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "Mips R5900 SA hazard"; }

  /// The number of emitted instructions between the last instruction that
  /// reads SA and the end of \p MBB, looking at most \p Limit instructions
  /// back, into the predecessors (the worst of them) at the start of a block.
  /// \p Limit if none reads SA.
  unsigned distanceFromEnd(const MachineBasicBlock &MBB, unsigned Limit,
                           unsigned Depth = 0) {
    unsigned N = 0;
    for (const MachineInstr &MI : reverse(MBB.instrs())) {
      if (N >= Limit)
        return Limit;
      if (!isEmitted(MI))
        continue;
      if (readsSA(MI))
        return N;
      ++N;
    }
    // The predecessors: the function entry has none.
    unsigned Worst = Limit;
    if (Depth < HazardDistance)
      for (const MachineBasicBlock *Pred : MBB.predecessors())
        Worst =
            std::min(Worst, N + distanceFromEnd(*Pred, Limit - N, Depth + 1));
    return Worst;
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    const MipsSubtarget &ST = MF.getSubtarget<MipsSubtarget>();
    if (!ST.isR5900())
      return false;
    const TargetInstrInfo &TII = *ST.getInstrInfo();
    bool Changed = false;
    for (MachineBasicBlock &MBB : MF) {
      for (MachineBasicBlock::instr_iterator I = MBB.instr_begin(),
                                             E = MBB.instr_end();
           I != E; ++I) {
        if (!writesSA(*I))
          continue;
        // The instructions before it in this block, then the predecessors.
        unsigned N = 0;
        bool Found = false;
        for (MachineBasicBlock::instr_iterator J = I; J != MBB.instr_begin();) {
          --J;
          if (N >= HazardDistance)
            break;
          if (!isEmitted(*J))
            continue;
          if (readsSA(*J)) {
            Found = true;
            break;
          }
          ++N;
        }
        if (!Found && N < HazardDistance) {
          unsigned Worst = HazardDistance;
          for (const MachineBasicBlock *Pred : MBB.predecessors())
            Worst =
                std::min(Worst, N + distanceFromEnd(*Pred, HazardDistance - N));
          Found = Worst < HazardDistance;
          N = Worst;
        }
        if (!Found)
          continue;
        // Inside a bundle (a delay slot), the nops go before the bundle.
        MachineBasicBlock::instr_iterator At = I;
        if (At->isBundledWithPred())
          At = getBundleStart(At);
        for (; N < HazardDistance; ++N, ++NumNops)
          BuildMI(MBB, At, I->getDebugLoc(), TII.get(Mips::NOP));
        Changed = true;
      }
    }
    return Changed;
  }
};

/// Whether \p A and \p B, both MTSAB or MTSAH, set SA to the same value. In
/// SSA form a virtual register has one value.
bool sameSAWrite(const MachineInstr &A, const MachineInstr &B) {
  if (A.getOpcode() != B.getOpcode() ||
      (A.getOpcode() != Mips::MTSAB && A.getOpcode() != Mips::MTSAH))
    return false;
  const MachineOperand &RA = A.getOperand(0), &RB = B.getOperand(0);
  Register R = RA.getReg();
  return R == RB.getReg() &&
         (R.isVirtual() || R == Mips::R0_64 || R == Mips::R0) &&
         A.getOperand(1).getImm() == B.getOperand(1).getImm();
}

class MipsR5900SARedundant : public MachineFunctionPass {
public:
  static char ID;
  MipsR5900SARedundant() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return "Mips R5900 redundant SA writes";
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    const MipsSubtarget &ST = MF.getSubtarget<MipsSubtarget>();
    if (!ST.isR5900())
      return false;
    const TargetRegisterInfo *TRI = ST.getRegisterInfo();
    bool Changed = false;
    for (MachineBasicBlock &MBB : MF) {
      // The last write of SA in this block.
      const MachineInstr *Last = nullptr;
      for (MachineInstr &MI : make_early_inc_range(MBB)) {
        if (Last && sameSAWrite(*Last, MI)) {
          MI.eraseFromParent();
          ++NumRedundant;
          Changed = true;
          continue;
        }
        if (MI.isInlineAsm() || MI.modifiesRegister(Mips::SAR, TRI))
          Last = writesSA(MI) ? &MI : nullptr;
      }
    }
    return Changed;
  }
};

} // namespace

char MipsR5900SARedundant::ID = 0;

FunctionPass *llvm::createMipsR5900SARedundantPass() {
  return new MipsR5900SARedundant();
}

char MipsR5900SAHazard::ID = 0;

FunctionPass *llvm::createMipsR5900SAHazardPass() {
  return new MipsR5900SAHazard();
}
