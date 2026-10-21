//===-- VUFrameLowering.h - VU frame lowering -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_VU_VUFRAMELOWERING_H
#define LLVM_LIB_TARGET_VU_VUFRAMELOWERING_H

#include "llvm/CodeGen/TargetFrameLowering.h"

namespace llvm {

/// Micro programs have no stack: VU data memory is laid out by the program.
class VUFrameLowering : public TargetFrameLowering {
public:
  VUFrameLowering()
      : TargetFrameLowering(StackGrowsDown, Align(16), 0, Align(16)) {}

  void emitPrologue(MachineFunction &MF, MachineBasicBlock &MBB) const override;
  void emitEpilogue(MachineFunction &MF, MachineBasicBlock &MBB) const override;

protected:
  bool hasFPImpl(const MachineFunction &MF) const override { return false; }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_VU_VUFRAMELOWERING_H
