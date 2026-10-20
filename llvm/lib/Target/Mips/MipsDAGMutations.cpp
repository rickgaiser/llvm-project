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
// scheduled between the definition of a value and its readers.
//
//===----------------------------------------------------------------------===//

#include "MipsDAGMutations.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/ScheduleDAGInstrs.h"

using namespace llvm;

#define DEBUG_TYPE "mips-dag-mutations"

namespace {

class MipsAccumulatorDAGMutation : public ScheduleDAGMutation {
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

void MipsAccumulatorDAGMutation::apply(ScheduleDAGInstrs *DAG) {
  // Only virtual registers are tracked, so this does nothing after register
  // allocation.
  const MachineRegisterInfo &MRI = DAG->MRI;
  DenseMap<MCRegister, AccumulatorValue> Live;
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
      MCRegister Acc = getAccumulator(MRI, MO.getReg());
      if (!Acc)
        continue;
      AccumulatorValue &Value = Live[Acc];
      // The previous value must be dead before the new one is defined.
      for (SUnit *Reader : Value.Readers)
        if (Reader != &SU && DAG->canAddEdge(&SU, Reader))
          DAG->addEdge(&SU, SDep(Reader, SDep::Artificial));
      Value.Reg = MO.getReg();
      Value.Readers.clear();
    }
  }
}

std::unique_ptr<ScheduleDAGMutation> llvm::createMipsAccumulatorDAGMutation() {
  return std::make_unique<MipsAccumulatorDAGMutation>();
}
