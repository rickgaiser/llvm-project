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
// In the same way it moves sums of products that are accumulated in HI and
// LO (PseudoR5900MULTAcc, PseudoR5900MADDAcc..., from MipsSETargetLowering)
// to HI1 and LO1 (MULT1, MADD1...) as a whole, such as one of the two sums
// of an unrolled loop: the two chains then run in parallel.
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
  MachineRegisterInfo *MRI = nullptr;
  TargetSchedModel SchedModel;

  bool usesAC1(const MachineInstr &MI) const;
  const MCSchedClassDesc *getSchedClass(const MachineInstr &MI) const;
  bool moveChains(MachineBasicBlock &MBB, SmallVectorImpl<int> &Use);

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

const MCSchedClassDesc *
MipsR5900Mult1::getSchedClass(const MachineInstr &MI) const {
  return SchedModel.resolveSchedClass(&MI);
}

/// The resource bound of a block with \p Use, in cycles times 2 so that it
/// is an integer: the resource groups have 2 units. The issue width is left
/// out, the R5900 rarely issues two instructions in every cycle.
static unsigned getBound(const TargetSchedModel &SchedModel,
                         ArrayRef<int> Use) {
  unsigned Cycles = 0;
  for (unsigned Idx = 1; Idx != Use.size(); ++Idx) {
    unsigned Units = SchedModel.getProcResource(Idx)->NumUnits;
    Cycles = std::max(Cycles, unsigned(std::max(Use[Idx], 0)) * 2 / Units);
  }
  return Cycles;
}

/// Move whole sums of products in HI and LO to HI1 and LO1, as many as lower
/// the resource bound (\p Use, updated), every other one. A chain starts at a
/// PseudoR5900MULTAcc and continues with the PseudoR5900MADDAcc that reads
/// its accumulator, which nothing else reads (not one that a loop carries).
bool MipsR5900Mult1::moveChains(MachineBasicBlock &MBB,
                                SmallVectorImpl<int> &Use) {
  SmallVector<SmallVector<MachineInstr *, 8>, 4> Chains;
  for (MachineInstr &MI : MBB) {
    if (MI.getOpcode() != Mips::PseudoR5900MULTAcc)
      continue;
    SmallVector<MachineInstr *, 8> Chain = {&MI};
    bool OK = true;
    while (true) {
      Register Acc = Chain.back()->getOperand(1).getReg();
      if (MRI->use_nodbg_empty(Acc))
        break;
      if (!MRI->hasOneNonDBGUse(Acc)) {
        OK = false;
        break;
      }
      MachineInstr *Next = &*MRI->use_instr_nodbg_begin(Acc);
      if (Next->getOpcode() != Mips::PseudoR5900MADDAcc ||
          Next->getParent() != &MBB) {
        OK = false;
        break;
      }
      Chain.push_back(Next);
    }
    if (OK && Chain.size() >= 2)
      Chains.push_back(std::move(Chain));
  }
  if (Chains.size() < 2)
    return false;

  // The change of the resource use per moved chain.
  const MCSchedModel *SM = SchedModel.getMCSchedModel();
  auto ClassOf = [&](unsigned Opc) {
    return SM->getSchedClassDesc(TII->get(Opc).getSchedClass());
  };
  unsigned NumKinds = Use.size();
  SmallVector<SmallVector<int, 16>, 4> Deltas;
  for (auto &Chain : Chains) {
    SmallVector<int, 16> Delta(NumKinds, 0);
    for (MachineInstr *MI : Chain) {
      bool First = MI->getOpcode() == Mips::PseudoR5900MULTAcc;
      addUse(ClassOf(First ? Mips::PseudoR5900MULT1Acc
                           : Mips::PseudoR5900MADD1Acc),
             Delta, 1);
      addUse(getSchedClass(*MI), Delta, -1);
    }
    Deltas.push_back(std::move(Delta));
  }
  // K chains moved: every other one, spread over the block.
  auto Moved = [&](unsigned K, unsigned J) {
    return (2 * J + 1) * Chains.size() / (2 * K);
  };
  auto BoundWith = [&](unsigned K) {
    SmallVector<int, 16> U(Use.begin(), Use.end());
    for (unsigned J = 0; J != K; ++J)
      for (unsigned Idx = 0; Idx != NumKinds; ++Idx)
        U[Idx] += Deltas[Moved(K, J)][Idx];
    return getBound(SchedModel, U);
  };
  unsigned BestK = 0, Best = BoundWith(0);
  for (unsigned K = 1; K <= Chains.size() / 2; ++K)
    if (unsigned Cycles = BoundWith(K); Cycles < Best) {
      Best = Cycles;
      BestK = K;
    }
  if (!BestK)
    return false;

  for (unsigned J = 0; J != BestK; ++J) {
    unsigned C = Moved(BestK, J);
    for (unsigned Idx = 0; Idx != NumKinds; ++Idx)
      Use[Idx] += Deltas[C][Idx];
    for (MachineInstr *MI : Chains[C]) {
      MRI->setRegClass(MI->getOperand(1).getReg(), &Mips::ACC64_1RegClass);
      MI->setDesc(TII->get(MI->getOpcode() == Mips::PseudoR5900MULTAcc
                               ? Mips::PseudoR5900MULT1Acc
                               : Mips::PseudoR5900MADD1Acc));
    }
  }
  return true;
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
    addUse(getSchedClass(MI), Use, 1);
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
  bool Changed = moveChains(MBB, Use);
  if (Mults.size() < 2)
    return Changed;

  // The change of the resource use per multiply that is moved.
  const MCSchedModel *SM = SchedModel.getMCSchedModel();
  SmallVector<int, 16> Delta(NumKinds, 0);
  addUse(SM->getSchedClassDesc(TII->get(Mips::MULT1).getSchedClass()), Delta,
         1);
  addUse(SM->getSchedClassDesc(TII->get(Mips::R5900_MULT).getSchedClass()),
         Delta, -1);

  // The resource bound with K multiplies moved.
  auto Bound = [&](unsigned K) {
    SmallVector<int, 16> U(NumKinds);
    for (unsigned Idx = 0; Idx != NumKinds; ++Idx)
      U[Idx] = Use[Idx] + int(K) * Delta[Idx];
    return getBound(SchedModel, U);
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
    return Changed;

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
