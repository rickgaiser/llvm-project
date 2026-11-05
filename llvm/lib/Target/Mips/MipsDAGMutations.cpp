//===- MipsDAGMutations.cpp - Mips ScheduleDAG mutations ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The R5900 fills a cache line that misses in the order of its words, so the
// loads of a line are faster in the order of their addresses (measured: 4%
// when a line is read descending). The load order mutation keeps the loads
// from the same base register in ascending offset order after register
// allocation, where the order costs no registers. A scalarized vector
// load is otherwise read descending.
//
// A prefetch only fetches a line into the cache: its order among the loads
// and stores does not change what they read or write. The prefetch mutation
// removes its memory order dependencies in an iteration for the software
// pipeliner (which leaves them out between iterations itself), which could
// otherwise not move a store into a later stage than the prefetch of the same
// array.
//
// There is one HI/LO pair. The accumulator order mutation makes a multiply or
// divide wait until the result before it has been read: the scheduler would
// otherwise start it early for its latency, and the old result would have to go
// through the stack.
//
//===----------------------------------------------------------------------===//

#include "MipsDAGMutations.h"
#include "MCTargetDesc/MipsMCTargetDesc.h"
#include "MipsRegisterInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/ScheduleDAGInstrs.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"

using namespace llvm;

#define DEBUG_TYPE "mips-dag-mutations"

namespace {

class MipsAccOrderDAGMutation : public ScheduleDAGMutation {
public:
  void apply(ScheduleDAGInstrs *DAG) override;
};

class MipsLoadOrderDAGMutation : public ScheduleDAGMutation {
public:
  void apply(ScheduleDAGInstrs *DAG) override;
};

class MipsUnorderedPrefetchDAGMutation : public ScheduleDAGMutation {
public:
  void apply(ScheduleDAGInstrs *DAG) override;
};

} // end anonymous namespace

void MipsLoadOrderDAGMutation::apply(ScheduleDAGInstrs *DAG) {
  // After register allocation, so that the order costs no registers. A load
  // is (def, base, offset); loads have the same base if no instruction in
  // between defines it.
  const TargetRegisterInfo *TRI = DAG->TRI;
  struct Load {
    MCRegister Base;
    unsigned Version;
    int64_t Offset;
    SUnit *SU;
  };
  SmallVector<Load, 32> Loads;
  DenseMap<MCRegister, unsigned> Version;
  for (SUnit &SU : DAG->SUnits) {
    const MachineInstr *MI = SU.getInstr();
    if (MI->mayLoad() && !MI->mayStore() && !MI->hasOrderedMemoryRef() &&
        MI->getNumOperands() >= 3 && MI->getOperand(1).isReg() &&
        MI->getOperand(1).getReg().isPhysical() && MI->getOperand(2).isImm()) {
      MCRegister Base = MI->getOperand(1).getReg().asMCReg();
      Loads.push_back(
          {Base, Version.lookup(Base), MI->getOperand(2).getImm(), &SU});
    }
    for (const MachineOperand &MO : MI->all_defs())
      if (MO.getReg().isPhysical())
        for (MCRegAliasIterator AI(MO.getReg(), TRI, true); AI.isValid(); ++AI)
          ++Version[*AI];
  }

  llvm::stable_sort(Loads, [](const Load &A, const Load &B) {
    return std::tie(A.Base, A.Version, A.Offset) <
           std::tie(B.Base, B.Version, B.Offset);
  });
  for (unsigned I = 1, E = Loads.size(); I < E; ++I) {
    const Load &Prev = Loads[I - 1], &Cur = Loads[I];
    if (Prev.Base == Cur.Base && Prev.Version == Cur.Version &&
        Prev.Offset != Cur.Offset && DAG->canAddEdge(Cur.SU, Prev.SU))
      DAG->addEdge(Cur.SU, SDep(Prev.SU, SDep::Artificial));
  }
}

std::unique_ptr<ScheduleDAGMutation> llvm::createMipsLoadOrderDAGMutation() {
  return std::make_unique<MipsLoadOrderDAGMutation>();
}

void MipsAccOrderDAGMutation::apply(ScheduleDAGInstrs *DAG) {
  struct Def {
    SUnit *SU;
    Register Reg;
  };
  SmallVector<Def, 4> Defs;
  DenseMap<Register, SmallVector<SUnit *, 2>> Readers;
  for (SUnit &SU : DAG->SUnits)
    for (const MachineOperand &MO : SU.getInstr()->operands()) {
      if (!MO.isReg() || !MO.getReg().isVirtual() ||
          DAG->MRI.getRegClass(MO.getReg()) != &Mips::ACC64RegClass)
        continue;
      if (MO.isDef())
        Defs.push_back({&SU, MO.getReg()});
      else
        Readers[MO.getReg()].push_back(&SU);
    }
  for (unsigned I = 1, E = Defs.size(); I < E; ++I)
    for (SUnit *Reader : Readers.lookup(Defs[I - 1].Reg))
      if (Reader != Defs[I].SU && DAG->canAddEdge(Defs[I].SU, Reader))
        DAG->addEdge(Defs[I].SU, SDep(Reader, SDep::Artificial));
}

std::unique_ptr<ScheduleDAGMutation> llvm::createMipsAccOrderDAGMutation() {
  return std::make_unique<MipsAccOrderDAGMutation>();
}

void MipsUnorderedPrefetchDAGMutation::apply(ScheduleDAGInstrs *DAG) {
  for (SUnit &SU : DAG->SUnits) {
    if (!DAG->TII->isPrefetch(*SU.getInstr()))
      continue;
    SmallVector<SDep, 4> Preds;
    for (const SDep &D : SU.Preds)
      if (D.getKind() == SDep::Order)
        Preds.push_back(D);
    for (const SDep &D : Preds)
      SU.removePred(D);
    SmallVector<std::pair<SUnit *, SDep>, 4> Succs;
    for (const SDep &D : SU.Succs)
      if (D.getKind() == SDep::Order) {
        SDep Pred = D;
        Pred.setSUnit(&SU);
        Succs.push_back({D.getSUnit(), Pred});
      }
    for (auto &[Succ, Pred] : Succs)
      Succ->removePred(Pred);
  }
}

std::unique_ptr<ScheduleDAGMutation>
llvm::createMipsUnorderedPrefetchDAGMutation() {
  return std::make_unique<MipsUnorderedPrefetchDAGMutation>();
}
