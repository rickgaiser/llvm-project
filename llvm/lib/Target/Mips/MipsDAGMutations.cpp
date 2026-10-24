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
//===----------------------------------------------------------------------===//

#include "MipsDAGMutations.h"
#include "MipsRegisterInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/ScheduleDAGInstrs.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"

using namespace llvm;

#define DEBUG_TYPE "mips-dag-mutations"

namespace {

class MipsLoadOrderDAGMutation : public ScheduleDAGMutation {
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
