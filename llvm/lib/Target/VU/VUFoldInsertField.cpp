//===-- VUFoldInsertField.cpp - Write inserted fields in place ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// An insertion of field f of s into field f of d is an FMAC instruction of its
// own (ADD<f>.<f> d, vf0, s; MULw.w d, s, vf0w for w). When s is the result of
// an upper instruction that computes each field from the same field of its
// operands, and nothing else reads s, that instruction writes field f of d
// instead:
//   MINIx.xyzw s, a, b; ...; MULw.w d, s, vf0w   ->   MINIx.w d, a, b; ...
// Nothing may read or write d between the two, since d.f now changes at the
// first. The registers are allocated: this runs after the pseudos are
// expanded, before the post-RA scheduler and VULoopScheduler, which pack the
// words of a loop again.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUMCTargetDesc.h"
#include "VU.h"
#include "VUSubtarget.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"

using namespace llvm;

#define DEBUG_TYPE "vu-fold-insert-field"

STATISTIC(NumFolded, "Number of field insertions written in place");

namespace {
class VUFoldInsertField : public MachineFunctionPass {
public:
  static char ID;
  VUFoldInsertField() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "VU Fold Insert Field"; }
  bool runOnMachineFunction(MachineFunction &MF) override;

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoVRegs();
  }

private:
  const TargetRegisterInfo *TRI = nullptr;
  bool fold(MachineInstr &Ins);
};
} // namespace

char VUFoldInsertField::ID = 0;

INITIALIZE_PASS(VUFoldInsertField, DEBUG_TYPE, "VU Fold Insert Field", false,
                false)

FunctionPass *llvm::createVUFoldInsertFieldPass() {
  return new VUFoldInsertField();
}

/// The index of the dest mask operand of an upper instruction that writes a
/// VF register, or 0 if \p MI is not one. Its first immediate operand is the
/// mask (an ACC input can come before it).
static unsigned getMaskIdx(const MachineInstr &MI) {
  if (MI.getDesc().TSFlags & 1 /* IsLower */ || MI.getNumExplicitDefs() != 1 ||
      !MI.getOperand(0).isReg() ||
      !VU::VFRegClass.contains(MI.getOperand(0).getReg()))
    return 0;
  for (unsigned I = 1, E = MI.getNumExplicitOperands(); I != E; ++I)
    if (MI.getOperand(I).isImm())
      return I;
  return 0;
}

/// The field (0 = x) that \p MI inserts from the same field of register
/// \p S, or -1: ADD<f>.<f> d, vf0, s (vf0.xyz is 0) or MULw.w d, s, vf0w
/// (vf0.w is 1).
static int getInsertedField(const MachineInstr &MI, Register &S) {
  static constexpr unsigned Add[] = {VU::ADDx, VU::ADDy, VU::ADDz, VU::ADDw};
  unsigned Opc = MI.getOpcode();
  for (int K = 0; K != 3; ++K)
    if (Opc == Add[K] && MI.getOperand(1).getImm() == (8 >> K) &&
        MI.getOperand(2).getReg() == VU::VF0) {
      S = MI.getOperand(3).getReg();
      return K;
    }
  if (Opc == VU::MULw && MI.getOperand(1).getImm() == 1 &&
      MI.getOperand(3).getReg() == VU::VF0) {
    S = MI.getOperand(2).getReg();
    return 3;
  }
  return -1;
}

bool VUFoldInsertField::fold(MachineInstr &Ins) {
  Register S;
  int F = getInsertedField(Ins, S);
  if (F < 0)
    return false;
  Register D = Ins.getOperand(0).getReg();
  if (S == D || S == VU::VF0)
    return false;
  MachineBasicBlock &MBB = *Ins.getParent();

  // s is not read after the insertion: up to a write of all of it, or the end
  // of the block when it is not live out.
  bool Redefined = false;
  for (MachineInstr &MI :
       make_range(std::next(MachineBasicBlock::iterator(Ins)), MBB.end())) {
    if (MI.readsRegister(S, TRI))
      return false;
    if (MI.definesRegister(S, TRI)) {
      Redefined = true;
      break;
    }
  }
  if (!Redefined)
    for (MachineBasicBlock *Succ : MBB.successors())
      if (Succ->isLiveIn(S))
        return false;

  // The instruction that writes s: nothing in between reads s, or reads or
  // writes d.
  MachineInstr *Def = nullptr;
  for (MachineInstr &MI : make_range(
           std::next(MachineBasicBlock::reverse_iterator(Ins)), MBB.rend())) {
    if (MI.isDebugInstr())
      continue;
    if (MI.modifiesRegister(S, TRI)) {
      Def = &MI;
      break;
    }
    if (MI.readsRegister(S, TRI) || MI.readsRegister(D, TRI) ||
        MI.modifiesRegister(D, TRI) || MI.isCall() ||
        MI.hasUnmodeledSideEffects())
      return false;
  }
  if (!Def || Def->getOperand(0).getReg() != S)
    return false;
  unsigned MaskIdx = getMaskIdx(*Def);
  if (!MaskIdx || !(Def->getOperand(MaskIdx).getImm() & (8 >> F)))
    return false;

  LLVM_DEBUG(dbgs() << "Fold into " << *Def << "  the insertion " << Ins);
  Def->getOperand(0).setReg(D);
  Def->getOperand(MaskIdx).setImm(8 >> F);
  // the other fields of s are no longer written, those of d are kept
  for (unsigned I = Def->getNumOperands();
       I-- > Def->getNumExplicitOperands();) {
    const MachineOperand &MO = Def->getOperand(I);
    if (MO.isReg() && MO.isImplicit() && MO.isUse() && MO.getReg() == S)
      Def->removeOperand(I);
  }
  Def->clearRegisterKills(D, TRI);
  // d keeps its other fields, which are undefined if they were for the
  // insertion.
  bool Undef = any_of(Ins.implicit_operands(), [&](const MachineOperand &MO) {
    return MO.isReg() && MO.isUse() && MO.getReg() == D && MO.isUndef();
  });
  Def->addOperand(MachineOperand::CreateReg(D, /*isDef=*/false, /*isImp=*/true,
                                            /*isKill=*/false, /*isDead=*/false,
                                            Undef));
  Ins.eraseFromParent();
  ++NumFolded;
  return true;
}

bool VUFoldInsertField::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;
  TRI = MF.getSubtarget().getRegisterInfo();
  bool Changed = false;
  for (MachineBasicBlock &MBB : MF)
    for (MachineInstr &MI : make_early_inc_range(MBB))
      Changed |= fold(MI);
  return Changed;
}
