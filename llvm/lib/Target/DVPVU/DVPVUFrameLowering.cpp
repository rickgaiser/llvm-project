//===-- DVPVUFrameLowering.cpp - DVPVU Frame Information ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the DVPVU implementation of TargetFrameLowering class.
//
//===----------------------------------------------------------------------===//

#include "DVPVUFrameLowering.h"
#include "DVPVU.h"
#include "DVPVUMachineFunctionInfo.h"
#include "DVPVUSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/RegisterScavenging.h"

using namespace llvm;

DVPVUFrameLowering::DVPVUFrameLowering()
    : TargetFrameLowering(StackGrowsDown,
                          /*StackAlignment=*/Align(16),
                          /*LocalAreaOffset=*/0,
                          /*TransientStackAlignment=*/Align(16)) {}

void DVPVUFrameLowering::emitPrologue(MachineFunction &MF,
                                       MachineBasicBlock &MBB) const {
  // VU has no stack in the traditional sense
  // Spills go to data memory at fixed addresses
  // Nothing to emit for prologue
}

void DVPVUFrameLowering::emitEpilogue(MachineFunction &MF,
                                       MachineBasicBlock &MBB) const {
  // Nothing to emit for epilogue
}

bool DVPVUFrameLowering::hasFPImpl(const MachineFunction &MF) const {
  // VU doesn't use a frame pointer
  return false;
}

MachineBasicBlock::iterator DVPVUFrameLowering::eliminateCallFramePseudoInstr(
    MachineFunction &MF, MachineBasicBlock &MBB,
    MachineBasicBlock::iterator MI) const {
  // Simply remove the pseudo instruction
  return MBB.erase(MI);
}

void DVPVUFrameLowering::determineCalleeSaves(MachineFunction &MF,
                                               BitVector &SavedRegs,
                                               RegScavenger *RS) const {
  TargetFrameLowering::determineCalleeSaves(MF, SavedRegs, RS);
  // VU typically doesn't save/restore registers across calls
  // since there's no traditional call stack
}
