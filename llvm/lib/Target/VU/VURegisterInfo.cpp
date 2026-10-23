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
  for (MCRegister R : {VU::I, VU::P, VU::R})
    Reserved.set(R);
  return Reserved;
}

bool VURegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator MI,
                                         int SPAdj, unsigned FIOperandNum,
                                         RegScavenger *RS) const {
  report_fatal_error("VU: stack frames are not supported");
}

const TargetRegisterClass *
VURegisterInfo::getLargestLegalSuperClass(const TargetRegisterClass *RC,
                                          const MachineFunction &MF) const {
  // There is only one ACC and one Q. When the register allocator splits a
  // value that overlaps another one, the part between its uses can move to a
  // VF register.
  if (RC == &VU::ACCRRegClass)
    return &VU::ACCVFRegClass;
  if (RC == &VU::QRRegClass)
    return &VU::QVFRegClass;
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
