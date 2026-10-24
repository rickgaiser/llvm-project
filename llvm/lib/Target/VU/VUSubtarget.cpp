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
#include "llvm/CodeGen/SingleRegClassMutations.h"

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
  // There is no spilling: a kernel that needs more registers than estimated
  // can not be compiled. Leave 3 of the 31 VF registers.
  Policy.RegPressureMargin = 10;
  // Iterative modulo scheduling unschedules instructions to make room, which
  // places the values of ACC and Q, with a single register each, much better
  // than swing modulo scheduling.
  Policy.UseIterativeScheduler = true;
  // A chain of dependent FMAC instructions takes 4 cycles per instruction,
  // and the loops are short, so the chain of one iteration spans more stages
  // than on other targets.
  Policy.MaxStages = 8;
  // A micro program is mostly its loops, and a loop body of many FMAC
  // instructions, such as a polynomial, is the common case, not the exception.
  Policy.MaxMII = -1;
  // Unrolling the kernel instead of copying long lived values: micro memory
  // has room for it, and a copy takes a lower slot and lengthens the
  // recurrence that it is on.
  Policy.UseMVE = true;
  // A long loop body that needs more registers when its iterations overlap
  // more is still much faster at a somewhat larger initiation interval than
  // not pipelined.
  Policy.IISearchRange = 40;
}

namespace {
/// The output dependence from a definition of P or I to the next one also
/// follows from a read in between. It is removed, so that the read is on the
/// path from one definition to the next, and the pipeliner orders it with
/// them instead of after both.
class ResultRegOrder : public ScheduleDAGMutation {
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
  }
};
} // namespace

void VUSubtarget::getSMSMutations(
    std::vector<std::unique_ptr<ScheduleDAGMutation>> &Mutations) const {
  Mutations.push_back(std::make_unique<ResultRegOrder>());
  // Keeps the uses of ACC and Q together with their definitions, like the
  // instructions of an ACC chain: MULA, MADDA..., MADD and DIV, ADDq.
  Mutations.push_back(
      createSingleRegChainStartMutation({&VU::ACCRRegClass, &VU::QRRegClass}));
}
