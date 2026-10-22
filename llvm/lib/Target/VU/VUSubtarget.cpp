//===-- VUSubtarget.cpp - VU subtarget information ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "VUSubtarget.h"
#include "MCTargetDesc/VUMCTargetDesc.h"
#include "llvm/CodeGen/MachinePipeliner.h"
#include "llvm/CodeGen/ScheduleDAGInstrs.h"
#include "llvm/CodeGen/ScheduleDAGMutation.h"

using namespace llvm;

#define DEBUG_TYPE "vu-subtarget"

#define GET_SUBTARGETINFO_TARGET_DESC
#define GET_SUBTARGETINFO_CTOR
#include "VUGenSubtargetInfo.inc"

VUSubtarget &VUSubtarget::initializeSubtargetDependencies(StringRef CPU,
                                                          StringRef FS) {
  if (CPU.empty())
    CPU = "generic";
  ParseSubtargetFeatures(CPU, /*TuneCPU=*/CPU, FS);
  return *this;
}

VUSubtarget::VUSubtarget(const Triple &TT, StringRef CPU, StringRef FS,
                         const TargetMachine &TM)
    : VUGenSubtargetInfo(TT, CPU, /*TuneCPU=*/CPU, FS),
      InstrInfo(initializeSubtargetDependencies(CPU, FS)), TLInfo(TM, *this),
      InstrItins(getInstrItineraryForCPU(CPU.empty() ? "generic" : CPU)) {}

void VUSubtarget::overridePipelinerPolicy(
    MachinePipelinerPolicy &Policy) const {
  Policy.ShouldLimitRegPressure = true;
  // Iterative modulo scheduling unschedules instructions to make room, which
  // places the values of ACC and Q, with a single register each, much better
  // than swing modulo scheduling.
  Policy.UseIterativeScheduler = true;
}

namespace {
/// Keeps the uses of ACC, Q, P and I together with their definitions for the
/// pipeliner, like the instructions of an ACC chain:
///   MULA, MADDA..., MADD  ->  MULA, ...
///   DIV, ADDq             ->  DIV, ...
/// - The output dependence from a definition of P or I to the next one
///   also follows from a read in between. It is removed, so that the read is
///   on the path from one definition to the next, and the pipeliner orders it
///   with them instead of after both.
/// - A group starts only when all its instructions can follow without
///   waiting for other operands. Otherwise the start would be scheduled as
///   early as possible and the rest later, which makes ACC or Q busy for
///   longer, or puts the group in two stages.
class ResultRegChains : public ScheduleDAGMutation {
  /// Delays the group that \p Start begins until the operands of all its
  /// instructions are ready. \p InGroup tells which registers the group
  /// passes on.
  static void delayGroupStart(ScheduleDAGInstrs *DAG, SUnit *Start,
                              function_ref<bool(Register)> InGroup) {
    // The earliest cycle of each instruction of the group, relative to Start.
    DenseMap<SUnit *, unsigned> Offset = {{Start, 0}};
    SmallVector<SUnit *, 8> Worklist = {Start};
    while (!Worklist.empty()) {
      SUnit *SU = Worklist.pop_back_val();
      for (const SDep &D : SU->Succs) {
        if (D.getKind() != SDep::Data || !InGroup(D.getReg()))
          continue;
        unsigned &O = Offset[D.getSUnit()];
        unsigned New = Offset[SU] + D.getLatency();
        if (New > O) {
          O = New;
          Worklist.push_back(D.getSUnit());
        }
      }
    }
    SmallVector<std::pair<SUnit *, unsigned>, 8> Edges;
    for (auto [SU, O] : Offset)
      for (const SDep &D : SU->Preds)
        if (D.getKind() == SDep::Data && !InGroup(D.getReg()) &&
            !Offset.count(D.getSUnit()) && D.getLatency() > O)
          Edges.push_back({D.getSUnit(), D.getLatency() - O});
    for (auto [Pred, Latency] : Edges) {
      SDep Dep(Pred, SDep::Artificial);
      Dep.setLatency(Latency);
      DAG->addEdge(Start, Dep);
    }
  }

public:
  void apply(ScheduleDAGInstrs *DAG) override {
    auto isReg = [](const SDep &D, SDep::Kind K, Register Reg) {
      return D.getKind() == K && D.getReg() == Reg;
    };
    SmallVector<std::pair<SUnit *, SDep>, 8> Remove;
    for (SUnit &SU : DAG->SUnits)
      for (const SDep &Succ : SU.Succs) {
        Register Reg =
            Succ.getKind() == SDep::Output ? Succ.getReg() : Register();
        if (Reg != VU::P && Reg != VU::I)
          continue;
        // A reader of this write that the next write depends on.
        SUnit *Next = Succ.getSUnit();
        bool ThroughRead = any_of(SU.Succs, [&](const SDep &Read) {
          return isReg(Read, SDep::Data, Reg) &&
                 any_of(Read.getSUnit()->Succs, [&](const SDep &D) {
                   return isReg(D, SDep::Anti, Reg) && D.getSUnit() == Next;
                 });
        });
        if (ThroughRead) {
          SDep Pred = Succ;
          Pred.setSUnit(&SU);
          Remove.push_back({Next, Pred});
        }
      }
    for (auto [SU, Pred] : Remove)
      SU->removePred(Pred);

    // Groups start with a write that does not read the register: MULA, DIV.
    for (const TargetRegisterClass *RC : {&VU::ACCRRegClass, &VU::QRRegClass}) {
      auto InGroup = [&](Register Reg) {
        return Reg.isVirtual() && DAG->MRI.getRegClass(Reg) == RC;
      };
      auto IsGroupOp = [&](const MachineOperand &MO) {
        return InGroup(MO.getReg());
      };
      for (SUnit &SU : DAG->SUnits) {
        const MachineInstr &MI = *SU.getInstr();
        if (any_of(MI.all_defs(), IsGroupOp) &&
            none_of(MI.all_uses(), IsGroupOp))
          delayGroupStart(DAG, &SU, InGroup);
      }
    }
  }
};
} // namespace

void VUSubtarget::getSMSMutations(
    std::vector<std::unique_ptr<ScheduleDAGMutation>> &Mutations) const {
  Mutations.push_back(std::make_unique<ResultRegChains>());
}
