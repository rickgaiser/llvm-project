//===- MipsOptimizeZeroExt.cpp - Sink zero extensions through PHIs --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// SelectionDAG lowers a narrow, zeroext return from a multi-predecessor PHI
// as an ANDi after the PHI. A predecessor containing an LBu (or LHu) has
// already zero extended the value. Moving the mask to the remaining edges
// keeps the ABI guarantee while avoiding it on the already-extended paths.
//
// This runs on machine SSA, before register allocation. Only single-successor
// predecessors may receive a mask: otherwise it could add work on unrelated
// outgoing edges.
//
//===----------------------------------------------------------------------===//

#include "Mips.h"
#include "MipsInstrInfo.h"
#include "MipsSubtarget.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"

using namespace llvm;

#define DEBUG_TYPE "mips-optimize-zeroext"

STATISTIC(NumSunkMasks, "Number of zero-extension masks sunk through PHIs");

namespace {
class MipsOptimizeZeroExt : public MachineFunctionPass {
public:
  static char ID;
  MipsOptimizeZeroExt() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return "Mips Optimize Zero Extensions";
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  static bool isAlreadyZeroExtended(Register Reg, uint32_t Mask,
                                    const MachineRegisterInfo &MRI);
  static bool trySinkMask(MachineInstr &MaskMI, MachineRegisterInfo &MRI,
                          const MipsInstrInfo &TII);
};
} // namespace

char MipsOptimizeZeroExt::ID = 0;

INITIALIZE_PASS(MipsOptimizeZeroExt, DEBUG_TYPE, "Mips Optimize Zero Extensions",
                false, false)

bool MipsOptimizeZeroExt::isAlreadyZeroExtended(
    Register Reg, uint32_t Mask, const MachineRegisterInfo &MRI) {
  // Look through unmodified full-register copies, but not subregister copies.
  for (unsigned Depth = 0; Depth != 4; ++Depth) {
    if (!Reg.isVirtual())
      return Reg == Mips::R0 || Reg == Mips::R0_64;

    const MachineInstr *Def = MRI.getVRegDef(Reg);
    if (!Def)
      return false;
    switch (Def->getOpcode()) {
    case Mips::LBu:
      return Mask >= 0xff;
    case Mips::LHu:
      return Mask >= 0xffff;
    case Mips::ANDi:
      return Def->getOperand(2).isImm() &&
             (static_cast<uint64_t>(Def->getOperand(2).getImm()) & ~Mask) == 0;
    case Mips::ORi:
      return Def->getOperand(1).isReg() &&
             Def->getOperand(1).getReg() == Mips::R0 &&
             Def->getOperand(2).isImm() &&
             Def->getOperand(2).getImm() >= 0 &&
             static_cast<uint64_t>(Def->getOperand(2).getImm()) <= Mask;
    case Mips::ADDiu:
      // ADDiu sign-extends its 16-bit immediate. Even though its encoding
      // permits 0xffff, that value produces -1 rather than 65535.
      return Def->getOperand(1).isReg() &&
             Def->getOperand(1).getReg() == Mips::R0 &&
             Def->getOperand(2).isImm() &&
             Def->getOperand(2).getImm() >= 0 &&
             Def->getOperand(2).getImm() <= 32767 &&
             static_cast<uint64_t>(Def->getOperand(2).getImm()) <= Mask;
    case TargetOpcode::COPY:
      if (Def->getOperand(1).getSubReg())
        return false;
      Reg = Def->getOperand(1).getReg();
      break;
    default:
      return false;
    }
  }
  return false;
}

bool MipsOptimizeZeroExt::trySinkMask(MachineInstr &MaskMI,
                                     MachineRegisterInfo &MRI,
                                     const MipsInstrInfo &TII) {
  if (MaskMI.getOpcode() != Mips::ANDi ||
      !MaskMI.getOperand(2).isImm())
    return false;

  int64_t Mask = MaskMI.getOperand(2).getImm();
  if (Mask != 0xff && Mask != 0xffff)
    return false;

  Register PhiReg = MaskMI.getOperand(1).getReg();
  if (!PhiReg.isVirtual() || !MRI.hasOneNonDBGUse(PhiReg))
    return false;

  MachineInstr *Phi = MRI.getVRegDef(PhiReg);
  if (!Phi || !Phi->isPHI() || Phi->getParent() != MaskMI.getParent())
    return false;

  MachineBasicBlock *Join = Phi->getParent();
  SmallVector<unsigned, 4> NeedsMask;
  bool HasExtendedIncoming = false;
  for (unsigned I = 1; I < Phi->getNumOperands(); I += 2) {
    Register Incoming = Phi->getOperand(I).getReg();
    MachineBasicBlock *Pred = Phi->getOperand(I + 1).getMBB();
    if (isAlreadyZeroExtended(Incoming, Mask, MRI)) {
      HasExtendedIncoming = true;
      continue;
    }
    if (Pred == Join || Pred->succ_size() != 1 ||
        *Pred->succ_begin() != Join)
      return false;
    NeedsMask.push_back(I);
  }

  // Never distribute a mask merely to move work when no predecessor already
  // produces the desired zero-extended value.
  if (!HasExtendedIncoming)
    return false;

  // Replacing the mask destination with the PHI must also respect the
  // destination's register-class constraints.
  Register MaskDst = MaskMI.getOperand(0).getReg();
  if (!MaskDst.isVirtual() ||
      !MRI.constrainRegClass(PhiReg, MRI.getRegClass(MaskDst)))
    return false;

  for (unsigned I : NeedsMask) {
    Register Incoming = Phi->getOperand(I).getReg();
    MachineBasicBlock *Pred = Phi->getOperand(I + 1).getMBB();
    Register Extended = MRI.createVirtualRegister(MRI.getRegClass(PhiReg));
    BuildMI(*Pred, Pred->getFirstTerminator(), MaskMI.getDebugLoc(),
            TII.get(Mips::ANDi), Extended)
        .addReg(Incoming)
        .addImm(Mask);
    Phi->getOperand(I).setReg(Extended);
  }

  MRI.replaceRegWith(MaskDst, PhiReg);
  MaskMI.eraseFromParent();
  ++NumSunkMasks;
  return true;
}

bool MipsOptimizeZeroExt::runOnMachineFunction(MachineFunction &MF) {
  // Sinking can duplicate a single ANDi across multiple predecessor blocks.
  // Do not increase static code size in size-optimized functions.
  if (skipFunction(MF.getFunction()) || MF.getFunction().hasOptSize() ||
      MF.getFunction().hasMinSize())
    return false;

  MachineRegisterInfo &MRI = MF.getRegInfo();
  if (!MRI.isSSA())
    return false;

  const MipsInstrInfo &TII = *MF.getSubtarget<MipsSubtarget>().getInstrInfo();
  bool Changed = false;
  for (MachineBasicBlock &MBB : MF)
    for (MachineInstr &MI : make_early_inc_range(MBB))
      if (MI.getOpcode() == Mips::ANDi)
        Changed |= trySinkMask(MI, MRI, TII);
  return Changed;
}

FunctionPass *llvm::createMipsOptimizeZeroExtPass() {
  return new MipsOptimizeZeroExt();
}
