//===- MipsR5900IssueModel.cpp - R5900 instruction issue model ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MipsR5900IssueModel.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSchedule.h"

using namespace llvm;

MipsR5900IssueModel::MipsR5900IssueModel(const TargetSchedModel &SchedModel,
                                         const TargetRegisterInfo &TRI)
    : SchedModel(&SchedModel), TRI(&TRI) {
  unsigned NumKinds = SchedModel.getNumProcResourceKinds();
  FirstUnit.resize(NumKinds);
  for (unsigned Idx = 1; Idx != NumKinds; ++Idx) {
    const MCProcResourceDesc *Res = SchedModel.getProcResource(Idx);
    FirstUnit[Idx] = Busy.size();
    if (!Res->SubUnitsIdxBegin)
      Busy.append(Res->NumUnits, 0);
  }
}

void MipsR5900IssueModel::startBlock(unsigned NewSlot) {
  Slot = NewSlot;
  Slot0MI = nullptr;
}

static bool isTrackedReg(const MachineOperand &MO,
                         const TargetRegisterInfo &TRI) {
  return MO.isReg() && MO.getReg().isPhysical() &&
         !TRI.isConstantPhysReg(MO.getReg());
}

unsigned MipsR5900IssueModel::getReadyCycle(const MachineInstr &MI) const {
  unsigned Ready = 0;
  for (unsigned I = 0, E = MI.getNumOperands(); I != E; ++I) {
    const MachineOperand &MO = MI.getOperand(I);
    if (!isTrackedReg(MO, *TRI) || !MO.readsReg())
      continue;
    for (MCRegUnit Unit : TRI->regunits(MO.getReg())) {
      auto It = Defs.find(static_cast<unsigned>(Unit));
      if (It == Defs.end())
        continue;
      const RegDef &Def = It->second;
      Ready = std::max(Ready, Def.Cycle + SchedModel->computeOperandLatency(
                                              Def.MI, Def.OpIdx, &MI, I));
    }
  }
  return Ready;
}

bool MipsR5900IssueModel::dependsOn(const MachineInstr &MI,
                                    const MachineInstr &DefMI) const {
  for (const MachineOperand &MO : DefMI.operands())
    if (isTrackedReg(MO, *TRI) && MO.isDef() &&
        (MI.readsRegister(MO.getReg(), TRI) ||
         MI.modifiesRegister(MO.getReg(), TRI)))
      return true;
  return false;
}

bool MipsR5900IssueModel::reserve(const MCSchedClassDesc *SC, unsigned Cycle,
                                  SmallVectorImpl<unsigned> &Units) const {
  if (!SC || !SC->isValid())
    return true;
  auto Entries = make_range(SchedModel->getWriteProcResBegin(SC),
                            SchedModel->getWriteProcResEnd(SC));
  for (const MCWriteProcResEntry &WPR : Entries) {
    unsigned Hold = WPR.ReleaseAtCycle - WPR.AcquireAtCycle;
    if (!Hold)
      continue;
    const MCProcResourceDesc *Res =
        SchedModel->getProcResource(WPR.ProcResourceIdx);
    // A group stands for one unit of any of its resources. TableGen also adds
    // the groups of each resource that is listed; those add nothing.
    unsigned Idx = WPR.ProcResourceIdx;
    ArrayRef<unsigned> Kinds(Idx);
    if (Res->SubUnitsIdxBegin) {
      Kinds = ArrayRef(Res->SubUnitsIdxBegin, Res->NumUnits);
      if (any_of(Entries, [&](const MCWriteProcResEntry &Other) {
            return is_contained(Kinds, Other.ProcResourceIdx);
          }))
        continue;
    }
    unsigned Free = ~0u;
    for (unsigned Kind : Kinds) {
      unsigned First = FirstUnit[Kind];
      unsigned End = First + SchedModel->getProcResource(Kind)->NumUnits;
      for (unsigned U = First; U != End; ++U)
        if (Units[U] <= Cycle && (Free == ~0u || Units[U] < Units[Free]))
          Free = U;
    }
    if (Free == ~0u)
      return false;
    Units[Free] = Cycle + Hold;
  }
  return true;
}

// Returns true if the resources of the first instruction of the block and an
// instruction of class SC fit in the cycle of the first one. The units of a
// group are assigned in either order, for example an ALU operation uses pipe 1
// when a multiply needs pipe 0.
bool MipsR5900IssueModel::fitsWithSlot0(const MCSchedClassDesc *SC) const {
  const MCSchedClassDesc *Slot0SC = SchedModel->resolveSchedClass(Slot0MI);
  SmallVector<unsigned, 16> Units(BusyBeforeSlot0);
  if (reserve(Slot0SC, Slot0Cycle, Units) && reserve(SC, Slot0Cycle, Units))
    return true;
  Units = BusyBeforeSlot0;
  return reserve(SC, Slot0Cycle, Units) && reserve(Slot0SC, Slot0Cycle, Units);
}

unsigned MipsR5900IssueModel::getResourceCycle(const MCSchedClassDesc *SC,
                                               unsigned Cycle,
                                               bool Pair) const {
  if (Pair && Cycle == Slot0Cycle) {
    if (fitsWithSlot0(SC))
      return Cycle;
    ++Cycle;
  }
  SmallVector<unsigned, 16> Units;
  for (;; ++Cycle) {
    Units = Busy;
    if (reserve(SC, Cycle, Units))
      return Cycle;
  }
}

unsigned MipsR5900IssueModel::getIssueCycle(const MachineInstr &MI) const {
  if (MI.isMetaInstruction() || MI.isBundle())
    return NextBlockCycle;
  bool Pair = Slot == 1 && Slot0MI;
  unsigned Cycle =
      std::max(Pair ? Slot0Cycle : NextBlockCycle, getReadyCycle(MI));
  // An instruction that depends on the first instruction of its block does
  // not issue with it, even if the latency allows it.
  if (Pair && Cycle == Slot0Cycle && dependsOn(MI, *Slot0MI))
    ++Cycle;
  return getResourceCycle(SchedModel->resolveSchedClass(&MI), Cycle, Pair);
}

bool MipsR5900IssueModel::wouldPair(const MachineInstr &MI) const {
  return Slot == 1 && Slot0MI && !MI.isMetaInstruction() && !MI.isBundle() &&
         getIssueCycle(MI) == Slot0Cycle;
}

unsigned MipsR5900IssueModel::issue(const MachineInstr &MI) {
  if (MI.isMetaInstruction() || MI.isBundle())
    return NextBlockCycle;
  const MCSchedClassDesc *SC = SchedModel->resolveSchedClass(&MI);
  unsigned Cycle = getIssueCycle(MI);
  if (Slot == 1 && Slot0MI && Cycle == Slot0Cycle) {
    // Reserve the units of both instructions in the order in which they fit.
    const MCSchedClassDesc *Slot0SC = SchedModel->resolveSchedClass(Slot0MI);
    SmallVector<unsigned, 16> Units(BusyBeforeSlot0);
    if (!reserve(Slot0SC, Cycle, Units) || !reserve(SC, Cycle, Units)) {
      Units = BusyBeforeSlot0;
      reserve(SC, Cycle, Units);
      reserve(Slot0SC, Cycle, Units);
    }
    Busy = Units;
  } else {
    if (Slot == 0)
      BusyBeforeSlot0 = Busy;
    reserve(SC, Cycle, Busy);
  }

  for (unsigned I = 0, E = MI.getNumOperands(); I != E; ++I) {
    const MachineOperand &MO = MI.getOperand(I);
    if (isTrackedReg(MO, *TRI) && MO.isDef())
      for (MCRegUnit Unit : TRI->regunits(MO.getReg()))
        Defs[static_cast<unsigned>(Unit)] = {&MI, I, Cycle};
  }

  NextBlockCycle = Cycle + 1;
  if (Slot == 0) {
    Slot0MI = &MI;
    Slot0Cycle = Cycle;
    Slot = 1;
  } else {
    Slot0MI = nullptr;
    Slot = 0;
  }
  return Cycle;
}
