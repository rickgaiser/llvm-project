//===- MipsR5900DeadDefs.cpp - R5900 unused multiply results --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The R5900 multiplies (MULT, MADD, PMULTH, PMADDH, ...) also write their
// result to rd, a few cycles later, and an instruction that writes the same
// register before then waits for it. In a chain of multiply-adds into HI and
// LO the rd results are mostly unused, and the register allocator gives them
// the registers that the next operands go to. This pass, before register
// allocation, makes an unused rd $zero, as the hand-written code does.
//
//===----------------------------------------------------------------------===//

#include "Mips.h"
#include "MipsInstrInfo.h"
#include "MipsSubtarget.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"

using namespace llvm;

#define DEBUG_TYPE "mips-r5900-dead-defs"

STATISTIC(NumZeroed, "Number of unused R5900 multiply results made $zero");

namespace {

bool isMultiply(unsigned Opc) {
  switch (Opc) {
  case Mips::R5900_MULT:
  case Mips::R5900_MULTU:
  case Mips::R5900_MADD:
  case Mips::R5900_MADDU:
  case Mips::MULT1:
  case Mips::MADD1:
  case Mips::PMULTW:
  case Mips::PMADDW:
  case Mips::PMSUBW:
  case Mips::PMULTUW:
  case Mips::PMADDUW:
  case Mips::PMULTH:
  case Mips::PMADDH:
  case Mips::PMSUBH:
  case Mips::PHMADH:
  case Mips::PHMSBH:
  // A PMADDH in a loop-carried accumulator (MipsR5900LoopAcc).
  case Mips::PseudoPMADDH:
    return true;
  default:
    return false;
  }
}

class MipsR5900DeadDefs : public MachineFunctionPass {
public:
  static char ID;
  MipsR5900DeadDefs() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return "Mips R5900 unused multiply results";
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    const MipsSubtarget &ST = MF.getSubtarget<MipsSubtarget>();
    if (!ST.isR5900())
      return false;
    MachineRegisterInfo &MRI = MF.getRegInfo();
    const TargetRegisterInfo *TRI = ST.getRegisterInfo();
    bool Changed = false;
    for (MachineBasicBlock &MBB : MF)
      for (MachineInstr &MI : MBB) {
        if (!isMultiply(MI.getOpcode()))
          continue;
        MachineOperand &MO = MI.getOperand(0);
        if (!MO.isReg() || !MO.isDef() || !MO.getReg().isVirtual() ||
            !MRI.use_nodbg_empty(MO.getReg()))
          continue;
        MCRegister Zero;
        switch (TRI->getRegSizeInBits(*MRI.getRegClass(MO.getReg()))) {
        case 32:
          Zero = Mips::R0;
          break;
        case 64:
          Zero = Mips::R0_64;
          break;
        case 128:
          Zero = Mips::R0_128;
          break;
        default:
          continue;
        }
        MO.setReg(Zero);
        MO.setIsDead();
        ++NumZeroed;
        Changed = true;
      }
    return Changed;
  }
};

} // namespace

char MipsR5900DeadDefs::ID = 0;

FunctionPass *llvm::createMipsR5900DeadDefsPass() {
  return new MipsR5900DeadDefs();
}
