//===- MipsDAGMutations.cpp - Mips ScheduleDAG mutations ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// An accumulator such as HI/LO is a register class with a single
// register, so only one value in it can be live at a time. The pre-RA
// scheduler does not know this: to hide the latency of a multiply it hoists
// the next multiply above the MFLO of the previous one, and every overlapping
// value then has to be spilled through a general purpose register and the
// stack. This mutation keeps the input order of the live ranges in such a
// register class: it adds an artificial edge from each reader of a value to
// the instruction that defines the next value. Other instructions can still be
// scheduled between the definition of a value and its readers, except for an
// instruction that writes the accumulator directly, such as an R5900 3-operand
// MULT with dead HI/LO results: it also ends the value.
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
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/ScheduleDAGInstrs.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"

using namespace llvm;

#define DEBUG_TYPE "mips-dag-mutations"

namespace {

class MipsAccumulatorDAGMutation : public ScheduleDAGMutation {
public:
  void apply(ScheduleDAGInstrs *DAG) override;
};

class MipsLoadOrderDAGMutation : public ScheduleDAGMutation {
public:
  void apply(ScheduleDAGInstrs *DAG) override;
};

/// The value that is live in an accumulator and its readers so far.
struct AccumulatorValue {
  Register Reg;
  SmallVector<SUnit *, 4> Readers;
};

} // end anonymous namespace

/// Return the register of a single-register class that \p Reg is allocated
/// to, or an invalid register if \p Reg is not a virtual register of such a
/// class.
static MCRegister getAccumulator(const MachineRegisterInfo &MRI, Register Reg) {
  if (!Reg.isVirtual())
    return MCRegister();
  const TargetRegisterClass *RC = MRI.getRegClass(Reg);
  if (RC->getNumRegs() != 1)
    return MCRegister();
  return RC->getRegister(0);
}

/// Keep the readers of \p Value before \p SU, which ends the value.
static void endValue(ScheduleDAGInstrs *DAG, AccumulatorValue &Value,
                     SUnit &SU) {
  for (SUnit *Reader : Value.Readers)
    if (Reader != &SU && DAG->canAddEdge(&SU, Reader))
      DAG->addEdge(&SU, SDep(Reader, SDep::Artificial));
}

void MipsAccumulatorDAGMutation::apply(ScheduleDAGInstrs *DAG) {
  // Only virtual registers are tracked, so this does nothing after register
  // allocation.
  const MachineRegisterInfo &MRI = DAG->MRI;
  const TargetRegisterInfo *TRI = DAG->TRI;
  DenseMap<MCRegister, AccumulatorValue> Live;
  for (SUnit &SU : DAG->SUnits)
    for (const MachineOperand &MO : SU.getInstr()->operands())
      if (MO.isReg())
        if (MCRegister Acc = getAccumulator(MRI, MO.getReg()))
          Live.try_emplace(Acc);
  if (Live.empty())
    return;

  for (SUnit &SU : DAG->SUnits) {
    const MachineInstr *MI = SU.getInstr();

    for (const MachineOperand &MO : MI->all_uses()) {
      MCRegister Acc = getAccumulator(MRI, MO.getReg());
      if (!Acc)
        continue;
      AccumulatorValue &Value = Live[Acc];
      // A value defined before the region.
      if (!Value.Reg)
        Value.Reg = MO.getReg();
      // The input already overlaps the live ranges, leave them.
      if (Value.Reg == MO.getReg())
        Value.Readers.push_back(&SU);
    }

    for (const MachineOperand &MO : MI->all_defs()) {
      // A direct write ends the value, and the next value is defined after
      // it. All readers of the value go before it, also those after it in
      // the input order.
      if (MO.getReg().isPhysical()) {
        for (auto &[Acc, Value] : Live) {
          if (!TRI->regsOverlap(MO.getReg(), Acc))
            continue;
          endValue(DAG, Value, SU);
          if (Value.Reg)
            for (MachineInstr &UseMI : MRI.use_nodbg_instructions(Value.Reg))
              if (SUnit *Reader = DAG->getSUnit(&UseMI))
                if (Reader != &SU && DAG->canAddEdge(&SU, Reader))
                  DAG->addEdge(&SU, SDep(Reader, SDep::Artificial));
          Value.Reg = Register();
          Value.Readers.assign(1, &SU);
        }
        continue;
      }
      MCRegister Acc = getAccumulator(MRI, MO.getReg());
      if (!Acc)
        continue;
      AccumulatorValue &Value = Live[Acc];
      // The previous value must be dead before the new one is defined.
      endValue(DAG, Value, SU);
      Value.Reg = MO.getReg();
      Value.Readers.clear();
    }
  }
}

std::unique_ptr<ScheduleDAGMutation> llvm::createMipsAccumulatorDAGMutation() {
  return std::make_unique<MipsAccumulatorDAGMutation>();
}

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
