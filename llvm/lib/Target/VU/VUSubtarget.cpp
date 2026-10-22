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
}

namespace {
/// Keeps the uses of ACC, Q, P and I together with their definitions for the
/// pipeliner, like the instructions of an ACC chain:
///   MULA, MADDA..., MADD  ->  MULA, ...
///   DIV, ADDq             ->  DIV, ...
/// - The output dependence from a definition to the next one also follows
///   from a read in between. It is removed, so that the read is on the path
///   from one definition to the next, and the pipeliner orders it with them
///   instead of after both.
/// - The next write of ACC is an upper instruction after the read, so it is
///   at least one word later.
class ResultRegChains : public ScheduleDAGMutation {
public:
  void apply(ScheduleDAGInstrs *DAG) override {
    auto isReg = [](const SDep &D, SDep::Kind K, Register Reg) {
      return D.getKind() == K && D.getReg() == Reg;
    };
    SmallVector<std::pair<SUnit *, SDep>, 8> Remove;
    SmallVector<std::pair<SUnit *, SUnit *>, 8> Add;
    for (SUnit &SU : DAG->SUnits)
      for (const SDep &Succ : SU.Succs) {
        if (isReg(Succ, SDep::Anti, VU::ACC))
          Add.push_back({&SU, Succ.getSUnit()});
        Register Reg =
            Succ.getKind() == SDep::Output ? Succ.getReg() : Register();
        if (Reg != VU::ACC && Reg != VU::Q && Reg != VU::P && Reg != VU::I)
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
    for (auto [Pred, Succ] : Add) {
      SDep Dep(Pred, SDep::Artificial);
      Dep.setLatency(1);
      DAG->addEdge(Succ, Dep);
    }
  }
};
} // namespace

void VUSubtarget::getSMSMutations(
    std::vector<std::unique_ptr<ScheduleDAGMutation>> &Mutations) const {
  Mutations.push_back(std::make_unique<ResultRegChains>());
}
