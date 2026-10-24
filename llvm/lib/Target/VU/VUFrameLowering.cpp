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

// Only spill slots are supported: they are at the top of VU data memory (see
// VURegisterInfo::eliminateFrameIndex), in the space that the program's layout
// reserves for them, "vu-spill-qw" quadwords (default none).
void VUFrameLowering::emitPrologue(MachineFunction &MF,
                                   MachineBasicBlock &MBB) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  const Function &F = MF.getFunction();
  for (int FI = MFI.getObjectIndexBegin(), E = MFI.getObjectIndexEnd(); FI != E;
       ++FI)
    if (!MFI.isDeadObjectIndex(FI) && !MFI.isSpillSlotObjectIndex(FI)) {
      F.getContext().diagnose(
          DiagnosticInfoUnsupported(F, "VU: stack objects are not supported"));
      return;
    }
  uint64_t SpillQW = MFI.getStackSize() / 16;
  uint64_t Limit = F.getFnAttributeAsParsedInteger("vu-spill-qw", 0);
  if (SpillQW > Limit)
    F.getContext().diagnose(DiagnosticInfoUnsupported(
        F, "VU: the spills need " + Twine(SpillQW) +
               " quadwords at the top of data memory, more than the " +
               Twine(Limit) + " reserved (\"vu-spill-qw\")"));
}

void VUFrameLowering::emitEpilogue(MachineFunction &MF,
                                   MachineBasicBlock &MBB) const {}
