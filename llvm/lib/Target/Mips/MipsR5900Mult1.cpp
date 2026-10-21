//===- MipsR5900Mult1.cpp - Use the R5900 pipe 1 multiplier ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The R5900 has a second multiplier, used by the 3-operand MULT1/MULTU1. A
// multiply occupies its multiplier for 2 cycles, and the two multipliers run in
// parallel, but MULT1 issues in pipe 1 with the loads and stores (measured).
// This pass moves some of the 3-operand multiplies of a block whose HI/LO
// results are dead to the pipe 1 multiplier, as many as lower the resource
// bound of the block given by the scheduling model, spread evenly over the
// block. A block that is limited by its loads and stores keeps its multiplies
// in pipe 0. The R5900 issue pass then places them so that their late result
// write does not stall the instructions in the same pipe.
//
//===----------------------------------------------------------------------===//

#include "Mips.h"
#include "MipsInstrInfo.h"
#include "MipsSubtarget.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetSchedule.h"
#include "llvm/Support/CommandLine.h"

#define DEBUG_TYPE "mips-r5900-mult1"

using namespace llvm;

STATISTIC(NumMult1, "Number of multiplies moved to the pipe 1 multiplier");

static cl::opt<bool>
    DisableR5900Mult1("disable-mips-r5900-mult1", cl::init(false), cl::Hidden,
                      cl::desc("Do not use the R5900 pipe 1 multiplier where "
                               "it lowers the resource bound of a block"));

namespace {

class MipsR5900Mult1 : public MachineFunctionPass {
public:
  static char ID;

  MipsR5900Mult1() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "R5900 pipe 1 multiplier"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  const MipsInstrInfo *TII = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  const MachineRegisterInfo *MRI = nullptr;
  TargetSchedModel SchedModel;

  bool usesAC1(const MachineInstr &MI) const;

  void addUse(const MCSchedClassDesc *SC, SmallVectorImpl<int> &Use,
              int Scale) const;
  bool runOnBlock(MachineBasicBlock &MBB);
};

} // end anonymous namespace

char MipsR5900Mult1::ID = 0;

INITIALIZE_PASS(MipsR5900Mult1, DEBUG_TYPE, "R5900 pipe 1 multiplier", false,
                false)

/// Return true if \p MI is a 3-operand multiply whose HI/LO results are dead.
static bool isCandidate(const MachineInstr &MI) {
  if (MI.getOpcode() != Mips::R5900_MULT && MI.getOpcode() != Mips::R5900_MULTU)
    return false;
  return all_of(MI.implicit_operands(), [](const MachineOperand &MO) {
    return !MO.isReg() || !MO.isDef() || MO.isDead();
  });
}

/// Add the resource cycles of \p SC times \p Scale to \p Use.
void MipsR5900Mult1::addUse(const MCSchedClassDesc *SC,
                            SmallVectorImpl<int> &Use, int Scale) const {
  if (!SC || !SC->isValid())
    return;
  for (const MCWriteProcResEntry &WPR :
       make_range(SchedModel.getWriteProcResBegin(SC),
                  SchedModel.getWriteProcResEnd(SC)))
    Use[WPR.ProcResourceIdx] +=
        Scale * int(WPR.ReleaseAtCycle - WPR.AcquireAtCycle);
}

/// Return true if \p MI reads or writes HI1/LO1, such as DIV1 or an MMI
/// multiply.
bool MipsR5900Mult1::usesAC1(const MachineInstr &MI) const {
  for (const MachineOperand &MO : MI.operands()) {
    if (!MO.isReg() || !MO.getReg())
      continue;
    if (MO.getReg().isPhysical()
            ? TRI->regsOverlap(MO.getReg(), Mips::AC1)
            : MRI->getRegClass(MO.getReg())->contains(Mips::AC1))
      return true;
  }
  return false;
}

bool MipsR5900Mult1::runOnBlock(MachineBasicBlock &MBB) {
  SmallVector<MachineInstr *, 16> Mults;
  unsigned NumKinds = SchedModel.getNumProcResourceKinds();
  SmallVector<int, 16> Use(NumKinds, 0);
  for (MachineInstr &MI : MBB) {
    if (usesAC1(MI))
      return false;
    // Most copies are coalesced away.
    if (MI.isMetaInstruction() || MI.isCopy() || MI.isPHI())
      continue;
    addUse(SchedModel.resolveSchedClass(&MI), Use, 1);
    // A multiply of the result of another one waits for it anyway.
    if (isCandidate(MI) && none_of(MI.uses(), [&](const MachineOperand &MO) {
          if (!MO.isReg() || !MO.getReg().isVirtual())
            return false;
          const MachineInstr *Def = MRI->getVRegDef(MO.getReg());
          return Def && Def->getParent() == &MBB &&
                 (Def->getOpcode() == Mips::R5900_MULT ||
                  Def->getOpcode() == Mips::R5900_MULTU);
        }))
      Mults.push_back(&MI);
  }
  if (Mults.size() < 2)
    return false;

  // The change of the resource use per multiply that is moved.
  const MCSchedModel *SM = SchedModel.getMCSchedModel();
  SmallVector<int, 16> Delta(NumKinds, 0);
  addUse(SM->getSchedClassDesc(TII->get(Mips::MULT1).getSchedClass()), Delta,
         1);
  addUse(SM->getSchedClassDesc(TII->get(Mips::R5900_MULT).getSchedClass()),
         Delta, -1);

  // The resource bound with K multiplies moved, in cycles times 2 so that it
  // is an integer: the resource groups have 2 units. The issue width is left
  // out, the R5900 rarely issues two instructions in every cycle.
  auto Bound = [&](unsigned K) {
    unsigned Cycles = 0;
    for (unsigned Idx = 1; Idx != NumKinds; ++Idx) {
      unsigned Units = SchedModel.getProcResource(Idx)->NumUnits;
      int Cost = Use[Idx] + int(K) * Delta[Idx];
      Cycles = std::max(Cycles, unsigned(Cost) * 2 / Units);
    }
    return Cycles;
  };
  unsigned BestK = 0, Best = Bound(0);
  for (unsigned K = 1; K <= Mults.size(); ++K) {
    unsigned Cycles = Bound(K);
    if (Cycles < Best) {
      Best = Cycles;
      BestK = K;
    }
  }
  if (!BestK)
    return false;

  // Spread the moved multiplies over the block.
  for (unsigned J = 0; J != BestK; ++J) {
    MachineInstr &MI = *Mults[(2 * J + 1) * Mults.size() / (2 * BestK)];
    unsigned Opc =
        MI.getOpcode() == Mips::R5900_MULT ? Mips::MULT1 : Mips::MULTU1;
    MachineInstr *NewMI = BuildMI(MBB, MI, MI.getDebugLoc(), TII->get(Opc))
                              .add(MI.getOperand(0))
                              .add(MI.getOperand(1))
                              .add(MI.getOperand(2));
    for (MachineOperand &MO : NewMI->implicit_operands())
      MO.setIsDead();
    NewMI->setFlags(MI.getFlags());
    MI.eraseFromParent();
    ++NumMult1;
  }
  return true;
}

bool MipsR5900Mult1::runOnMachineFunction(MachineFunction &MF) {
  const MipsSubtarget &STI = MF.getSubtarget<MipsSubtarget>();
  if (DisableR5900Mult1 || !STI.isR5900() || skipFunction(MF.getFunction()))
    return false;

  TII = STI.getInstrInfo();
  TRI = STI.getRegisterInfo();
  MRI = &MF.getRegInfo();
  SchedModel.init(&STI);

  bool Changed = false;
  for (MachineBasicBlock &MBB : MF)
    Changed |= runOnBlock(MBB);
  return Changed;
}

FunctionPass *llvm::createMipsR5900Mult1Pass() { return new MipsR5900Mult1(); }
