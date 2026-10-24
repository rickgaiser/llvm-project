//===- SingleRegClassMutations.cpp - Values of one-register classes -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/SingleRegClassMutations.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/ScheduleDAGInstrs.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"

using namespace llvm;

namespace {

class SingleRegClassOrder : public ScheduleDAGMutation {
public:
  void apply(ScheduleDAGInstrs *DAG) override;
};

class SingleRegChainStart : public ScheduleDAGMutation {
  SmallVector<const TargetRegisterClass *, 4> Classes;

public:
  SingleRegChainStart(ArrayRef<const TargetRegisterClass *> Classes)
      : Classes(Classes) {}
  void apply(ScheduleDAGInstrs *DAG) override;
};

/// The value that is live in a register (the last one defined) and the
/// readers of the values since the last start of one.
struct LiveValue {
  Register Reg;
  SmallVector<SUnit *, 8> Readers;
};

} // end anonymous namespace

/// Returns the register of a single-register class that \p Reg is allocated
/// to, or an invalid register if \p Reg is not a virtual register of such a
/// class.
static MCRegister getSingleReg(const MachineRegisterInfo &MRI, Register Reg) {
  if (!Reg.isVirtual())
    return MCRegister();
  const TargetRegisterClass *RC = MRI.getRegClass(Reg);
  if (RC->getNumRegs() != 1)
    return MCRegister();
  return RC->getRegister(0);
}

/// Keeps the readers of \p Value before \p SU.
static void orderReaders(ScheduleDAGInstrs *DAG, LiveValue &Value, SUnit &SU) {
  for (SUnit *Reader : Value.Readers)
    if (Reader != &SU && DAG->canAddEdge(&SU, Reader))
      DAG->addEdge(&SU, SDep(Reader, SDep::Artificial));
}

void SingleRegClassOrder::apply(ScheduleDAGInstrs *DAG) {
  // Only virtual registers are tracked, so this does nothing after register
  // allocation.
  const MachineRegisterInfo &MRI = DAG->MRI;
  const TargetRegisterInfo *TRI = DAG->TRI;
  DenseMap<MCRegister, LiveValue> Live;
  for (SUnit &SU : DAG->SUnits)
    for (const MachineOperand &MO : SU.getInstr()->operands())
      if (MO.isReg())
        if (MCRegister R = getSingleReg(MRI, MO.getReg()))
          Live.try_emplace(R);
  if (Live.empty())
    return;

  SmallVector<MCRegister, 2> Read;
  for (SUnit &SU : DAG->SUnits) {
    const MachineInstr *MI = SU.getInstr();
    Read.clear();
    for (const MachineOperand &MO : MI->all_uses())
      if (MCRegister R = getSingleReg(MRI, MO.getReg()))
        if (!is_contained(Read, R))
          Read.push_back(R);

    for (const MachineOperand &MO : MI->all_defs()) {
      // A direct write ends the value, and the next value is defined after
      // it. All readers of the value go before it, also those after it in
      // the input order.
      if (MO.getReg().isPhysical()) {
        for (auto &[R, Value] : Live) {
          if (!TRI->regsOverlap(MO.getReg(), R))
            continue;
          orderReaders(DAG, Value, SU);
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
      MCRegister R = getSingleReg(MRI, MO.getReg());
      if (!R)
        continue;
      LiveValue &Value = Live[R];
      Value.Reg = MO.getReg();
      // A value that does not continue one (such as MULA, not MADDA) starts
      // after the readers of the values before it.
      if (!is_contained(Read, R)) {
        orderReaders(DAG, Value, SU);
        Value.Readers.clear();
      }
    }
    for (MCRegister R : Read)
      Live[R].Readers.push_back(&SU);
  }
}

/// Delays the chain that \p Start begins until the operands of all its
/// instructions are ready: an artificial edge from each operand to Start.
static void delayChainStart(ScheduleDAGInstrs *DAG, SUnit *Start,
                            function_ref<bool(Register)> InChain) {
  // The earliest cycle of each instruction of the chain, relative to Start.
  DenseMap<SUnit *, unsigned> Offset = {{Start, 0}};
  SmallVector<SUnit *, 8> Worklist = {Start};
  while (!Worklist.empty()) {
    SUnit *SU = Worklist.pop_back_val();
    for (const SDep &D : SU->Succs) {
      if (D.getKind() != SDep::Data || !InChain(D.getReg()))
        continue;
      unsigned &O = Offset[D.getSUnit()];
      unsigned New = Offset[SU] + D.getLatency();
      if (New > O) {
        O = New;
        Worklist.push_back(D.getSUnit());
      }
    }
  }
  // One edge from each operand to Start, with the largest delay (a second
  // edge from the same instruction would not be added), in the order of the
  // instructions.
  DenseMap<SUnit *, unsigned> Delay;
  for (auto [SU, O] : Offset)
    for (const SDep &D : SU->Preds)
      if (D.getKind() == SDep::Data && !InChain(D.getReg()) &&
          !Offset.count(D.getSUnit()) && D.getLatency() > O) {
        unsigned &L = Delay[D.getSUnit()];
        L = std::max(L, D.getLatency() - O);
      }
  SmallVector<std::pair<SUnit *, unsigned>, 8> Edges(Delay.begin(),
                                                     Delay.end());
  llvm::sort(Edges, [](const auto &A, const auto &B) {
    return A.first->NodeNum < B.first->NodeNum;
  });
  for (auto [Pred, Latency] : Edges) {
    SDep Dep(Pred, SDep::Artificial);
    Dep.setLatency(Latency);
    DAG->addEdge(Start, Dep);
  }
}

void SingleRegChainStart::apply(ScheduleDAGInstrs *DAG) {
  for (const TargetRegisterClass *RC : Classes) {
    auto InChain = [&](Register Reg) {
      return Reg.isVirtual() && DAG->MRI.getRegClass(Reg) == RC;
    };
    auto IsChainOp = [&](const MachineOperand &MO) {
      return InChain(MO.getReg());
    };
    // A chain starts with a write of the register that does not read it.
    for (SUnit &SU : DAG->SUnits) {
      const MachineInstr &MI = *SU.getInstr();
      if (any_of(MI.all_defs(), IsChainOp) && none_of(MI.all_uses(), IsChainOp))
        delayChainStart(DAG, &SU, InChain);
    }
  }
}

std::unique_ptr<ScheduleDAGMutation> llvm::createSingleRegClassOrderMutation() {
  return std::make_unique<SingleRegClassOrder>();
}

std::unique_ptr<ScheduleDAGMutation> llvm::createSingleRegChainStartMutation(
    ArrayRef<const TargetRegisterClass *> Classes) {
  return std::make_unique<SingleRegChainStart>(Classes);
}
