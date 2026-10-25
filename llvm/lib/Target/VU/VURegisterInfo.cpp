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
#include "VUSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
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
  // CF: the clipping flags, ordered by its implicit operands only.
  for (MCRegister R : {VU::I, VU::P, VU::R, VU::CF})
    Reserved.set(R);
  return Reserved;
}

// There is no stack: the spill slots are the quadwords at the top of data
// memory (1023 down on VU1, 255 down on VU0), addressed from VI0. The frame
// offsets are negative, in bytes.
bool VURegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator MI,
                                         int SPAdj, unsigned FIOperandNum,
                                         RegScavenger *RS) const {
  MachineFunction &MF = *MI->getMF();
  int FI = MI->getOperand(FIOperandNum).getIndex();
  int64_t Offset = MF.getFrameInfo().getObjectOffset(FI);
  int64_t Top = MF.getSubtarget<VUSubtarget>().hasVU1() ? 1024 : 256;
  MI->getOperand(FIOperandNum).ChangeToImmediate(Top + Offset / 16);
  return false;
}

const TargetRegisterClass *
VURegisterInfo::getLargestLegalSuperClass(const TargetRegisterClass *RC,
                                          const MachineFunction &MF) const {
  // There is only one ACC, one Q and one VI1 for FCAND. When the register
  // allocator splits a
  // value that overlaps another one, the part between its uses can move to a
  // VF register.
  if (RC == &VU::ACCRRegClass)
    return &VU::ACCVFRegClass;
  if (RC == &VU::QRRegClass)
    return &VU::QVFRegClass;
  // A clipping flag test result (VI1) can move to another VI register.
  if (RC == &VU::VI1RRegClass)
    return &VU::VIRegClass;
  return RC;
}

unsigned VURegisterInfo::getRegPressureSetLimit(const MachineFunction &MF,
                                                unsigned Idx) const {
  // ACCVF and QVF put ACC and Q in the pressure sets of VF, which counts them
  // as two more registers for VF values. They hold their own values, and the
  // values of ACC and Q count in these sets too, so only the VF registers are
  // available.
  for (const int *PSet = getRegClassPressureSets(&VU::VFRegClass); *PSet != -1;
       ++PSet)
    if (static_cast<unsigned>(*PSet) == Idx)
      return VU::VFRegClass.getNumRegs();
  return VUGenRegisterInfo::getRegPressureSetLimit(MF, Idx);
}

Register VURegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  return VU::VI0;
}
