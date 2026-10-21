//===-- VURegisterInfo.cpp - VU register information ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "VURegisterInfo.h"
#include "MCTargetDesc/VUMCTargetDesc.h"
#include "VUFrameLowering.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/ErrorHandling.h"

#define GET_REGINFO_TARGET_DESC
#include "VUGenRegisterInfo.inc"

using namespace llvm;

// BAL and JALR link through VI15.
VURegisterInfo::VURegisterInfo() : VUGenRegisterInfo(VU::VI15) {}

const MCPhysReg *
VURegisterInfo::getCalleeSavedRegs(const MachineFunction *MF) const {
  // Micro programs are not called, so nothing is preserved.
  static const MCPhysReg None[] = {0};
  return None;
}

BitVector VURegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  BitVector Reserved(getNumRegs());
  // VF0 is (0, 0, 0, 1) and VI0 is 0.
  Reserved.set(VU::VF0);
  Reserved.set(VU::VI0);
  for (MCRegister R : {VU::ACC, VU::I, VU::Q, VU::P, VU::R})
    Reserved.set(R);
  return Reserved;
}

bool VURegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator MI,
                                         int SPAdj, unsigned FIOperandNum,
                                         RegScavenger *RS) const {
  report_fatal_error("VU: stack frames are not supported");
}

Register VURegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  return VU::VI0;
}
