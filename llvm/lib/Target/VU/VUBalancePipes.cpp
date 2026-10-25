//===-- VUBalancePipes.cpp - Field insertions in the less busy pipe -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// An insertion of field f + 1 of s into field f of d (x from y, y from z, z
// from w) is either an upper instruction (INSERT_FIELD: ADD<f+1>.<f> d, vf0,
// s) or a lower one (INSERT_ROT: MR32.<f> d, s). Both are exact and take 4
// cycles. The initiation interval of a pipelined loop is at least the number
// of words of the busier pipe, so in a single block loop, before the
// pipeliner, insertions move to the lower pipe while that has fewer
// instructions than the upper one.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUMCTargetDesc.h"
#include "VU.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSchedule.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

#define DEBUG_TYPE "vu-balance-pipes"

STATISTIC(NumMoved, "Number of field insertions moved to the lower pipe");

static cl::opt<bool> Enable("vu-enable-balance-pipes", cl::Hidden,
                            cl::init(true),
                            cl::desc("Field insertions in the less busy pipe"));

namespace {
class VUBalancePipes : public MachineFunctionPass {
public:
  static char ID;
  VUBalancePipes() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "VU Balance Pipes"; }
  bool runOnMachineFunction(MachineFunction &MF) override;
};
} // namespace

char VUBalancePipes::ID = 0;

INITIALIZE_PASS(VUBalancePipes, DEBUG_TYPE, "VU Balance Pipes", false, false)

FunctionPass *llvm::createVUBalancePipesPass() { return new VUBalancePipes(); }

bool VUBalancePipes::runOnMachineFunction(MachineFunction &MF) {
  if (!Enable || skipFunction(MF.getFunction()))
    return false;
  const TargetSubtargetInfo &STI = MF.getSubtarget();
  const TargetInstrInfo &TII = *STI.getInstrInfo();
  TargetSchedModel SchedModel;
  SchedModel.init(&STI);
  if (!SchedModel.hasInstrSchedModel())
    return false;
  const MCSchedModel &SM = *SchedModel.getMCSchedModel();
  unsigned UpperRes = 0, LowerRes = 0;
  for (unsigned I = 1, E = SM.getNumProcResourceKinds(); I != E; ++I) {
    StringRef Name = SM.getProcResource(I)->Name;
    if (Name == "VUUpperRes")
      UpperRes = I;
    else if (Name == "VULowerRes")
      LowerRes = I;
  }
  if (!UpperRes || !LowerRes)
    return false;

  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    if (!MBB.isSuccessor(&MBB))
      continue;
    unsigned Upper = 0, Lower = 0;
    SmallVector<MachineInstr *, 8> Candidates;
    for (MachineInstr &MI : MBB) {
      if (MI.isDebugInstr() || MI.isPHI() || MI.isCopy())
        continue;
      const MCSchedClassDesc *SC = SchedModel.resolveSchedClass(&MI);
      for (const MCWriteProcResEntry &W : make_range(
               STI.getWriteProcResBegin(SC), STI.getWriteProcResEnd(SC))) {
        if (W.ProcResourceIdx == UpperRes)
          Upper += W.ReleaseAtCycle;
        else if (W.ProcResourceIdx == LowerRes)
          Lower += W.ReleaseAtCycle;
      }
      if (MI.getOpcode() == VU::INSERT_FIELD) {
        unsigned Field = MI.getOperand(3).getImm();
        if (Field < 3 && MI.getOperand(4).getImm() == Field + 1)
          Candidates.push_back(&MI);
      }
    }
    for (MachineInstr *MI : Candidates) {
      if (Upper <= Lower + 1)
        break;
      MI->setDesc(TII.get(VU::INSERT_ROT));
      MI->removeOperand(4);
      --Upper;
      ++Lower;
      ++NumMoved;
      Changed = true;
    }
  }
  return Changed;
}
