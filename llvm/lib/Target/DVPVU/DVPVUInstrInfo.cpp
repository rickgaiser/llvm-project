//===-- DVPVUInstrInfo.cpp - DVPVU Instruction Information ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the DVPVU implementation of the TargetInstrInfo class.
//
//===----------------------------------------------------------------------===//

#include "DVPVUInstrInfo.h"
#include "DVPVU.h"
#include "DVPVUSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define GET_REGINFO_ENUM
#include "DVPVUGenRegisterInfo.inc"

#define GET_INSTRINFO_ENUM
#define GET_INSTRINFO_CTOR_DTOR
#include "DVPVUGenInstrInfo.inc"

DVPVUInstrInfo::DVPVUInstrInfo(const DVPVUSubtarget &STI)
    : DVPVUGenInstrInfo(STI, RI, DVPVU::ADJCALLSTACKDOWN,
                        DVPVU::ADJCALLSTACKUP),
      RI() {}

void DVPVUInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                                  MachineBasicBlock::iterator MI,
                                  const DebugLoc &DL, Register DestReg,
                                  Register SrcReg, bool KillSrc,
                                  bool RenamableDest, bool RenamableSrc) const {
  // VF register copy: use MR32 or MOVE instruction
  if (DVPVU::VFRegsRegClass.contains(DestReg, SrcReg)) {
    // Use MOVE.xyzw vf_dst, vf_src (DEST = 0xF for xyzw)
    BuildMI(MBB, MI, DL, get(DVPVU::MOVE), DestReg)
        .addImm(0xF) // dest mask = xyzw
        .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }

  // VI register copy: use IADDIU with 0
  if (DVPVU::VIRegsRegClass.contains(DestReg, SrcReg)) {
    // IADDIU vi_dst, vi_src, 0
    BuildMI(MBB, MI, DL, get(DVPVU::IADDIi), DestReg)
        .addReg(SrcReg, getKillRegState(KillSrc))
        .addImm(0);
    return;
  }

  llvm_unreachable("Cannot copy between register classes");
}

void DVPVUInstrInfo::storeRegToStackSlot(MachineBasicBlock &MBB,
                                          MachineBasicBlock::iterator MI,
                                          Register SrcReg, bool IsKill,
                                          int FrameIndex,
                                          const TargetRegisterClass *RC,
                                          Register VReg,
                                          MachineInstr::MIFlag Flags) const {
  DebugLoc DL;
  if (MI != MBB.end())
    DL = MI->getDebugLoc();

  if (RC == &DVPVU::VFRegsRegClass || RC == &DVPVU::VFRegsNoVF0RegClass) {
    // SQ vf_src, offset(vi_base) - store quadword
    BuildMI(MBB, MI, DL, get(DVPVU::SQ))
        .addReg(SrcReg, getKillRegState(IsKill))
        .addImm(0xF) // dest = xyzw
        .addFrameIndex(FrameIndex)
        .addImm(0);
  } else if (RC == &DVPVU::VIRegsRegClass || RC == &DVPVU::VIRegsNoVI0RegClass) {
    // ISW vi_src, offset(vi_base) - store integer word
    BuildMI(MBB, MI, DL, get(DVPVU::ISW))
        .addReg(SrcReg, getKillRegState(IsKill))
        .addFrameIndex(FrameIndex)
        .addImm(0);
  } else {
    llvm_unreachable("Cannot store this register class to stack slot");
  }
}

void DVPVUInstrInfo::loadRegFromStackSlot(MachineBasicBlock &MBB,
                                           MachineBasicBlock::iterator MI,
                                           Register DestReg, int FrameIndex,
                                           const TargetRegisterClass *RC,
                                           Register VReg,
                                           MachineInstr::MIFlag Flags) const {
  DebugLoc DL;
  if (MI != MBB.end())
    DL = MI->getDebugLoc();

  if (RC == &DVPVU::VFRegsRegClass || RC == &DVPVU::VFRegsNoVF0RegClass) {
    // LQ vf_dst, offset(vi_base) - load quadword
    BuildMI(MBB, MI, DL, get(DVPVU::LQ), DestReg)
        .addImm(0xF) // dest = xyzw
        .addFrameIndex(FrameIndex)
        .addImm(0);
  } else if (RC == &DVPVU::VIRegsRegClass || RC == &DVPVU::VIRegsNoVI0RegClass) {
    // ILW vi_dst, offset(vi_base) - load integer word
    BuildMI(MBB, MI, DL, get(DVPVU::ILW), DestReg)
        .addFrameIndex(FrameIndex)
        .addImm(0);
  } else {
    llvm_unreachable("Cannot load this register class from stack slot");
  }
}

bool DVPVUInstrInfo::analyzeBranch(MachineBasicBlock &MBB,
                                    MachineBasicBlock *&TBB,
                                    MachineBasicBlock *&FBB,
                                    SmallVectorImpl<MachineOperand> &Cond,
                                    bool AllowModify) const {
  // TODO: Implement branch analysis
  return true;
}

unsigned DVPVUInstrInfo::insertBranch(MachineBasicBlock &MBB,
                                       MachineBasicBlock *TBB,
                                       MachineBasicBlock *FBB,
                                       ArrayRef<MachineOperand> Cond,
                                       const DebugLoc &DL,
                                       int *BytesAdded) const {
  assert(TBB && "insertBranch must not be told to insert a fallthrough");

  if (Cond.empty()) {
    // Unconditional branch
    BuildMI(&MBB, DL, get(DVPVU::B)).addMBB(TBB);
    if (BytesAdded)
      *BytesAdded = 8; // 64-bit instruction
    return 1;
  }

  // TODO: Implement conditional branches
  llvm_unreachable("Conditional branches not yet implemented");
}

unsigned DVPVUInstrInfo::removeBranch(MachineBasicBlock &MBB,
                                       int *BytesRemoved) const {
  // TODO: Implement branch removal
  return 0;
}
