//===-- DVPVURegisterInfo.cpp - DVPVU Register Information ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the DVPVU implementation of the TargetRegisterInfo class.
//
//===----------------------------------------------------------------------===//

#include "DVPVURegisterInfo.h"
#include "DVPVU.h"
#include "DVPVUSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define GET_REGINFO_ENUM
#define GET_REGINFO_TARGET_DESC
#include "DVPVUGenRegisterInfo.inc"

DVPVURegisterInfo::DVPVURegisterInfo() : DVPVUGenRegisterInfo(/*RA=*/0) {}

const MCPhysReg *
DVPVURegisterInfo::getCalleeSavedRegs(const MachineFunction *MF) const {
  // VU has no traditional calling convention - minimal callee-saved regs
  static const MCPhysReg CalleeSavedRegs[] = {
      0 // End of list
  };
  return CalleeSavedRegs;
}

BitVector DVPVURegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  BitVector Reserved(getNumRegs());

  // VF0 is constant (0, 0, 0, 1)
  Reserved.set(DVPVU::VF0);

  // VI0 is constant 0
  Reserved.set(DVPVU::VI0);

  // Special registers are reserved
  Reserved.set(DVPVU::ACC);
  Reserved.set(DVPVU::I_REG);
  Reserved.set(DVPVU::Q);
  Reserved.set(DVPVU::P);
  Reserved.set(DVPVU::R);
  Reserved.set(DVPVU::SF);
  Reserved.set(DVPVU::MAC);
  Reserved.set(DVPVU::CF);

  return Reserved;
}

const TargetRegisterClass *
DVPVURegisterInfo::getPointerRegClass(unsigned Kind) const {
  // VI registers are used for addressing
  return &DVPVU::VIRegsRegClass;
}

bool DVPVURegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator MI,
                                             int SPAdj, unsigned FIOperandNum,
                                             RegScavenger *RS) const {
  MachineInstr &Instr = *MI;
  MachineBasicBlock &MBB = *Instr.getParent();
  MachineFunction &MF = *MBB.getParent();
  const MachineFrameInfo &MFI = MF.getFrameInfo();

  int FrameIndex = Instr.getOperand(FIOperandNum).getIndex();
  int64_t Offset = MFI.getObjectOffset(FrameIndex);

  // Add the offset from the frame index operand
  if (Instr.getOperand(FIOperandNum + 1).isImm())
    Offset += Instr.getOperand(FIOperandNum + 1).getImm();

  // Replace the frame index with VI0 (base address 0) and offset
  // VU memory is addressed from 0, so frame objects are at their offsets
  Instr.getOperand(FIOperandNum).ChangeToRegister(DVPVU::VI0, false);
  Instr.getOperand(FIOperandNum + 1).ChangeToImmediate(Offset);

  return false;
}

Register DVPVURegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  // VU doesn't have a traditional frame pointer
  // Use VI15 as a pseudo frame pointer if needed
  return DVPVU::VI15;
}
