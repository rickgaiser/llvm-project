//===-- VUFrameLowering.cpp - VU frame lowering ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "VUFrameLowering.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Function.h"

using namespace llvm;

void VUFrameLowering::emitPrologue(MachineFunction &MF,
                                   MachineBasicBlock &MBB) const {
  if (MF.getFrameInfo().hasStackObjects())
    MF.getFunction().getContext().diagnose(DiagnosticInfoUnsupported(
        MF.getFunction(), "VU: stack objects are not supported"));
}

void VUFrameLowering::emitEpilogue(MachineFunction &MF,
                                   MachineBasicBlock &MBB) const {}
