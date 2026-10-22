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
#include "llvm/CodeGen/DFAPacketizer.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/Support/ErrorHandling.h"

#define GET_INSTRINFO_CTOR_DTOR
#include "VUGenInstrInfo.inc"

using namespace llvm;

#include "VUGenDFAPacketizer.inc"

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

bool VUInstrInfo::isReMaterializableImpl(const MachineInstr &MI) const {
  // LOADF32 clobbers I, but I is only live between the instructions of an
  // expanded pseudo, so the constant can be materialized anywhere. This lets
  // MachineLICM hoist constants out of loops.
  if (MI.getOpcode() == VU::LOADF32)
    return true;
  return TargetInstrInfo::isReMaterializableImpl(MI);
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

DFAPacketizer *
VUInstrInfo::CreateTargetScheduleState(const TargetSubtargetInfo &STI) const {
  const InstrItineraryData *II = STI.getInstrItineraryData();
  return static_cast<const VUSubtarget &>(STI).createDFAPacketizer(II);
}

static bool isVIReg(Register Reg, const MachineRegisterInfo &MRI) {
  if (Reg.isVirtual())
    return MRI.getRegClass(Reg) == &VU::VIRegClass;
  return VU::VIRegClass.contains(Reg);
}

/// Returns true if a conditional branch may directly follow \p MI when it
/// reads a VI register written by \p MI.
static bool isFlagCheck(unsigned Opc) {
  switch (Opc) {
  case VU::FCAND:
  case VU::FCEQ:
  case VU::FCGET:
  case VU::FCOR:
  case VU::FMAND:
  case VU::FMEQ:
  case VU::FMOR:
  case VU::FSAND:
  case VU::FSEQ:
  case VU::FSOR:
    return true;
  default:
    return false;
  }
}

/// Returns the number of words after \p MI until its result in \p Reg can be
/// read without a stall.
static unsigned getResultLatency(const MachineInstr &MI, Register Reg) {
  switch (MI.getOpcode()) {
  // Pseudos, expanded after register allocation.
  case VU::LOADF32:
  case VU::FNEG:
    return 5; // LOI; FMAC
  case VU::SPLAT:
    return 5; // FMAC; FMAC
  case VU::FDIV32:
  case VU::FSQRT32:
    return 12; // DIV/SQRT; WAITQ; FMAC

  // Q and P are only read after a WAITQ or WAITP, which waits for the result
  // and may share its word with the reader.
  case VU::DIV:
  case VU::SQRT:
    return 7;
  case VU::RSQRT:
    return 13;
  case VU::WAITQ:
  case VU::WAITP:
    return 0;

  // EFU (VU1).
  case VU::ESADD:
    return 11;
  case VU::ESQRT:
  case VU::ESUM:
  case VU::ERCPR:
    return 12;
  case VU::ERSADD:
  case VU::ELENG:
  case VU::ERSQRT:
    return 18;
  case VU::ERLENG:
    return 24;
  case VU::ESIN:
    return 29;
  case VU::EEXP:
    return 44;
  case VU::EATAN:
  case VU::EATANxy:
  case VU::EATANxz:
    return 54;

  // Integer loads go through the FMAC pipeline.
  case VU::ILW:
  case VU::ILWR:
    return 4;
  }

  if (MI.isCopyLike() || MI.isMetaInstruction())
    return 0;
  // ACC is forwarded between FMAC instructions; I, R and VI results are
  // available to the next word.
  if (Reg == VU::ACC || Reg == VU::I || Reg == VU::R ||
      isVIReg(Reg, MI.getMF()->getRegInfo()))
    return 1;
  // FMAC pipeline.
  return 4;
}

std::optional<unsigned> VUInstrInfo::getOperandLatency(
    const InstrItineraryData *ItinData, const MachineInstr &DefMI,
    unsigned DefIdx, const MachineInstr &UseMI, unsigned UseIdx) const {
  Register Reg = DefMI.getOperand(DefIdx).getReg();
  const MachineOperand &UseMO = UseMI.getOperand(UseIdx);

  // An implicit use of the VF register that UseMI writes only keeps its other
  // fields alive. Writes to other fields do not stall.
  if (UseMO.isImplicit() && VU::VFRegClass.contains(Reg) &&
      UseMI.definesRegister(Reg, /*TRI=*/nullptr))
    return 1;

  unsigned Latency = getResultLatency(DefMI, Reg);
  // A conditional branch needs one word between the instruction that sets
  // its VI register and itself, except after a flag check.
  if (UseMI.isConditionalBranch() && !isFlagCheck(DefMI.getOpcode()) &&
      isVIReg(Reg, DefMI.getMF()->getRegInfo()))
    Latency = std::max(Latency, 2u);
  return Latency;
}

bool VUInstrInfo::getBaseAndOffsetPosition(const MachineInstr &MI,
                                           unsigned &BasePos,
                                           unsigned &OffsetPos) const {
  switch (MI.getOpcode()) {
  case VU::LQ:
  case VU::SQ:
  case VU::ILW:
  case VU::ISW:
    OffsetPos = 2;
    BasePos = 3;
    return true;
  default:
    return false;
  }
}

bool VUInstrInfo::getMemOperandsWithOffsetWidth(
    const MachineInstr &MI, SmallVectorImpl<const MachineOperand *> &BaseOps,
    int64_t &Offset, bool &OffsetIsScalable, LocationSize &Width) const {
  unsigned BasePos, OffsetPos;
  if (!getBaseAndOffsetPosition(MI, BasePos, OffsetPos))
    return false;
  // Masked and integer accesses are treated as accessing the whole quadword.
  BaseOps.push_back(&MI.getOperand(BasePos));
  Offset = MI.getOperand(OffsetPos).getImm() * 16;
  OffsetIsScalable = false;
  Width = LocationSize::precise(16);
  return true;
}

bool VUInstrInfo::getIncrementValue(const MachineInstr &MI, int &Value) const {
  switch (MI.getOpcode()) {
  case VU::IADDI:
  case VU::IADDIU:
    Value = MI.getOperand(2).getImm() * 16;
    return true;
  case VU::ISUBIU:
    Value = -MI.getOperand(2).getImm() * 16;
    return true;
  default:
    return false;
  }
}

bool VUInstrInfo::areMemAccessesTriviallyDisjoint(
    const MachineInstr &MIa, const MachineInstr &MIb) const {
  SmallVector<const MachineOperand *, 1> BaseA, BaseB;
  int64_t OffsetA, OffsetB;
  bool Scalable;
  LocationSize WidthA = LocationSize::precise(0),
               WidthB = LocationSize::precise(0);
  if (!getMemOperandsWithOffsetWidth(MIa, BaseA, OffsetA, Scalable, WidthA) ||
      !getMemOperandsWithOffsetWidth(MIb, BaseB, OffsetB, Scalable, WidthB) ||
      BaseA[0]->getReg() != BaseB[0]->getReg())
    return false;
  // A register written by MIa itself is not a common base.
  if (MIa.modifiesRegister(BaseA[0]->getReg(), /*TRI=*/nullptr) ||
      MIb.modifiesRegister(BaseB[0]->getReg(), /*TRI=*/nullptr))
    return false;
  int64_t Lo = std::min(OffsetA, OffsetB), Hi = std::max(OffsetA, OffsetB);
  int64_t LoWidth = OffsetA <= OffsetB ? WidthA.getValue().getFixedValue()
                                       : WidthB.getValue().getFixedValue();
  return Lo + LoWidth <= Hi;
}

namespace {
/// A single block loop that ends in a conditional branch back to itself.
class VUPipelinerLoopInfo : public TargetInstrInfo::PipelinerLoopInfo {
  SmallVector<const MachineInstr *, 2> CondDefs;
  SmallVector<MachineOperand, 3> Cond;

public:
  VUPipelinerLoopInfo(ArrayRef<const MachineInstr *> CondDefs,
                      ArrayRef<MachineOperand> Cond)
      : CondDefs(CondDefs), Cond(Cond) {}

  // Keep the instructions that compute the loop condition in stage 0.
  bool shouldIgnoreForPipelining(const MachineInstr *MI) const override {
    return is_contained(CondDefs, MI);
  }

  // The branch "if (Cond) goto epilogue" is inserted after a copy of the
  // loop condition computation; Cond is normalized for that use.
  std::optional<bool> createTripCountGreaterCondition(
      int TC, MachineBasicBlock &MBB,
      SmallVectorImpl<MachineOperand> &CondParam) override {
    CondParam = Cond;
    return {};
  }

  void setPreheader(MachineBasicBlock *NewPreheader) override {}
  void adjustTripCount(int TripCountAdjust) override {}
};
} // namespace

std::unique_ptr<TargetInstrInfo::PipelinerLoopInfo>
VUInstrInfo::analyzeLoopForPipelining(MachineBasicBlock *LoopBB) const {
  MachineBasicBlock *TBB = nullptr, *FBB = nullptr;
  SmallVector<MachineOperand, 4> Cond;
  if (analyzeBranch(*LoopBB, TBB, FBB, Cond) || !FBB || TBB == FBB)
    return nullptr;
  assert((TBB == LoopBB || FBB == LoopBB) && "not a single block loop");
  // Branch to the exit when the condition is true.
  if (TBB == LoopBB)
    reverseBranchCondition(Cond);

  const MachineRegisterInfo &MRI = LoopBB->getParent()->getRegInfo();
  SmallVector<const MachineInstr *, 2> CondDefs;
  for (const MachineOperand &MO : drop_begin(Cond)) {
    if (!MO.isReg() || !MO.getReg().isVirtual())
      continue;
    const MachineInstr *Def = MRI.getVRegDef(MO.getReg());
    if (Def->isPHI())
      return nullptr;
    if (Def->getParent() == LoopBB)
      CondDefs.push_back(Def);
  }
  return std::make_unique<VUPipelinerLoopInfo>(CondDefs, Cond);
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
