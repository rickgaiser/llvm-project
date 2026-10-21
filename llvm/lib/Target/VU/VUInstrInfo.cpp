//===-- VUInstrInfo.cpp - VU instruction information ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "VUInstrInfo.h"
#include "MCTargetDesc/VUMCTargetDesc.h"
#include "VUSubtarget.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/IR/Constants.h"
#include "llvm/Support/ErrorHandling.h"

#define GET_INSTRINFO_CTOR_DTOR
#include "VUGenInstrInfo.inc"

using namespace llvm;

// Destination masks.
static constexpr unsigned MaskX = 8, MaskW = 1, MaskXYZ = 14, MaskXYZW = 15;

VUInstrInfo::VUInstrInfo(const VUSubtarget &STI) : VUGenInstrInfo(STI, RI) {}

void VUInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator MBBI,
                              const DebugLoc &DL, Register DestReg,
                              Register SrcReg, bool KillSrc, bool RenamableDest,
                              bool RenamableSrc) const {
  if (VU::VFRegClass.contains(DestReg, SrcReg)) {
    BuildMI(MBB, MBBI, DL, get(VU::MOVE), DestReg)
        .addImm(MaskXYZW)
        .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }
  if (VU::VIRegClass.contains(DestReg, SrcReg)) {
    BuildMI(MBB, MBBI, DL, get(VU::IADDIU), DestReg)
        .addReg(SrcReg, getKillRegState(KillSrc))
        .addImm(0);
    return;
  }
  report_fatal_error("VU: unsupported register copy");
}

void VUInstrInfo::storeRegToStackSlot(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI, Register SrcReg,
    bool isKill, int FrameIndex, const TargetRegisterClass *RC, Register VReg,
    MachineInstr::MIFlag Flags) const {
  report_fatal_error("VU: register spilling is not supported");
}

void VUInstrInfo::loadRegFromStackSlot(MachineBasicBlock &MBB,
                                       MachineBasicBlock::iterator MBBI,
                                       Register DestReg, int FrameIdx,
                                       const TargetRegisterClass *RC,
                                       Register VReg, unsigned SubReg,
                                       MachineInstr::MIFlag Flags) const {
  report_fatal_error("VU: register spilling is not supported");
}

bool VUInstrInfo::expandPostRAPseudo(MachineInstr &MI) const {
  MachineBasicBlock &MBB = *MI.getParent();
  const DebugLoc &DL = MI.getDebugLoc();
  auto Build = [&](unsigned Opc) { return BuildMI(MBB, MI, DL, get(Opc)); };
  auto BuildDef = [&](unsigned Opc, Register Dst) {
    return BuildMI(MBB, MI, DL, get(Opc), Dst);
  };

  switch (MI.getOpcode()) {
  default:
    return false;

  case VU::LOADF32: {
    // LOI c; ADDi.x d, vf0, i
    const APFloat &C = MI.getOperand(1).getFPImm()->getValueAPF();
    Build(VU::LOI).addImm(C.bitcastToAPInt().getZExtValue());
    BuildDef(VU::ADDi, MI.getOperand(0).getReg()).addImm(MaskX).addReg(VU::VF0);
    break;
  }

  case VU::SPLAT: {
    // ADDx.xyz d, vf0, s; MULx.w d, vf0, s (vf0.w is 1)
    Register D = MI.getOperand(0).getReg();
    Register S = MI.getOperand(1).getReg();
    BuildDef(VU::ADDx, D).addImm(MaskXYZ).addReg(VU::VF0).addReg(S);
    BuildDef(VU::MULx, D)
        .addImm(MaskW)
        .addReg(VU::VF0)
        .addReg(S)
        .addReg(D, RegState::Implicit);
    break;
  }

  case VU::INSERT_FIELD: {
    // ADDx.<field> d, vf0, s, or MULx.w d, vf0, s for the w field.
    Register D = MI.getOperand(0).getReg();
    Register S = MI.getOperand(2).getReg();
    unsigned Field = MI.getOperand(3).getImm();
    BuildDef(Field == 3 ? VU::MULx : VU::ADDx, D)
        .addImm(8 >> Field)
        .addReg(VU::VF0)
        .addReg(S)
        .addReg(D, RegState::Implicit);
    break;
  }

  case VU::FNEG:
    // LOI -1.0; MULi.xyzw d, s, i
    Build(VU::LOI).addImm(0xbf800000);
    BuildDef(VU::MULi, MI.getOperand(0).getReg())
        .addImm(MaskXYZW)
        .add(MI.getOperand(1));
    break;

  case VU::FDIV32:
  case VU::FSQRT32: {
    // DIV q, a.x, b.x / SQRT q, a.x; WAITQ; ADDq.x d, vf0, q
    if (MI.getOpcode() == VU::FDIV32)
      Build(VU::DIV)
          .add(MI.getOperand(1))
          .addImm(0)
          .add(MI.getOperand(2))
          .addImm(0);
    else
      Build(VU::SQRT).add(MI.getOperand(1)).addImm(0);
    Build(VU::WAITQ);
    BuildDef(VU::ADDq, MI.getOperand(0).getReg()).addImm(MaskX).addReg(VU::VF0);
    break;
  }
  }

  MI.eraseFromParent();
  return true;
}

/// Returns the opposite condition of a conditional branch, or 0.
static unsigned getOppositeBranch(unsigned Opc) {
  switch (Opc) {
  case VU::IBEQ:
    return VU::IBNE;
  case VU::IBNE:
    return VU::IBEQ;
  case VU::IBLTZ:
    return VU::IBGEZ;
  case VU::IBGEZ:
    return VU::IBLTZ;
  case VU::IBGTZ:
    return VU::IBLEZ;
  case VU::IBLEZ:
    return VU::IBGTZ;
  default:
    return 0;
  }
}

bool VUInstrInfo::analyzeBranch(MachineBasicBlock &MBB, MachineBasicBlock *&TBB,
                                MachineBasicBlock *&FBB,
                                SmallVectorImpl<MachineOperand> &Cond,
                                bool AllowModify) const {
  MachineBasicBlock::iterator I = MBB.getLastNonDebugInstr();
  if (I == MBB.end() || !isUnpredicatedTerminator(*I))
    return false; // Falls through.

  // Collect the terminators, last one first.
  SmallVector<MachineInstr *, 2> Terms;
  while (true) {
    if (!I->isBranch())
      return true; // E.g. the end of the program.
    Terms.push_back(&*I);
    if (I == MBB.begin())
      break;
    --I;
    if (I->isDebugInstr() || !isUnpredicatedTerminator(*I))
      break;
  }
  if (Terms.size() > 2)
    return true;

  auto ParseCond = [&](MachineInstr &MI) {
    if (!getOppositeBranch(MI.getOpcode()))
      return false;
    Cond.push_back(MachineOperand::CreateImm(MI.getOpcode()));
    for (unsigned Op = 0, E = MI.getNumExplicitOperands() - 1; Op != E; ++Op)
      Cond.push_back(MI.getOperand(Op));
    TBB = MI.getOperand(MI.getNumExplicitOperands() - 1).getMBB();
    return true;
  };

  MachineInstr &Last = *Terms[0];
  if (Terms.size() == 1) {
    if (Last.getOpcode() == VU::B) {
      TBB = Last.getOperand(0).getMBB();
      return false;
    }
    return !ParseCond(Last);
  }

  // Conditional branch followed by an unconditional one.
  if (Last.getOpcode() != VU::B || !ParseCond(*Terms[1]))
    return true;
  FBB = Last.getOperand(0).getMBB();
  return false;
}

unsigned VUInstrInfo::removeBranch(MachineBasicBlock &MBB,
                                   int *BytesRemoved) const {
  unsigned Count = 0;
  MachineBasicBlock::iterator I = MBB.end();
  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr())
      continue;
    if (!I->isBranch())
      break;
    I->eraseFromParent();
    I = MBB.end();
    ++Count;
  }
  if (BytesRemoved)
    *BytesRemoved = Count * 8;
  return Count;
}

unsigned VUInstrInfo::insertBranch(MachineBasicBlock &MBB,
                                   MachineBasicBlock *TBB,
                                   MachineBasicBlock *FBB,
                                   ArrayRef<MachineOperand> Cond,
                                   const DebugLoc &DL, int *BytesAdded) const {
  assert(TBB && "insertBranch must not be told to insert a fallthrough");
  unsigned Count = 0;
  if (Cond.empty()) {
    BuildMI(&MBB, DL, get(VU::B)).addMBB(TBB);
    Count = 1;
  } else {
    MachineInstrBuilder MIB = BuildMI(&MBB, DL, get(Cond[0].getImm()));
    for (const MachineOperand &MO : Cond.drop_front())
      MIB.add(MO);
    MIB.addMBB(TBB);
    Count = 1;
    if (FBB) {
      BuildMI(&MBB, DL, get(VU::B)).addMBB(FBB);
      ++Count;
    }
  }
  if (BytesAdded)
    *BytesAdded = Count * 8;
  return Count;
}

bool VUInstrInfo::reverseBranchCondition(
    SmallVectorImpl<MachineOperand> &Cond) const {
  Cond[0].setImm(getOppositeBranch(Cond[0].getImm()));
  return false;
}
