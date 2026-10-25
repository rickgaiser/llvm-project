//===-- VUInvariantFields.cpp - Fields set once before a loop -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A merge in a loop of the fields of a value computed in it and of a value
// from outside of it, such as float4(l + amb.xyz, amb.w), is a MOVE in every
// iteration (MERGE_FIELDS). An upper instruction writes only the fields of its
// mask and keeps the others, so the instruction that computes the value can
// write its fields into a register whose other fields were set before the
// loop and are never written in it:
//   loop: %t = ADD.xyzw %l, %amb; %d = MERGE_FIELDS %t, %amb, w
//   ->
//   loop: %r = PHI [%amb, outside], [%d, loop]
//         %d = ADD.xyz %l, %amb, implicit %r (tied)
// The tied use keeps field w of %r; it does not stall the ADD (an implicit
// use of the register that an instruction writes, VUInstrInfo).
//
// Only when the merged value is stored and read by nothing else (a GIF
// register): the pipeliner then keeps it short, and it dies before the next
// ADD writes the register again (vumax S6: 20.94 -> 19.96 cycles a vertex).
// A value that more instructions read can live into the next iteration; the
// tied register is then copied in every iteration, in front of the ADD, which
// costs more than the MOVE (the model demo's mlit: 4 % slower).
//
// This runs on SSA form, after MachineLICM (which takes the outside value out
// of the loop) and before the pipeliner, on single block loops.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUMCTargetDesc.h"
#include "VU.h"
#include "VUSubtarget.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

#define DEBUG_TYPE "vu-invariant-fields"

STATISTIC(NumFolded, "Number of merges with fields set before the loop");

static cl::opt<bool> Enable("vu-invariant-fields", cl::Hidden, cl::init(true),
                            cl::desc("Fields set once before a loop"));

namespace {
class VUInvariantFields : public MachineFunctionPass {
public:
  static char ID;
  VUInvariantFields() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "VU Invariant Fields"; }
  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  MachineRegisterInfo *MRI = nullptr;
  const TargetInstrInfo *TII = nullptr;
  bool fold(MachineInstr &Merge);
};
} // namespace

char VUInvariantFields::ID = 0;

INITIALIZE_PASS(VUInvariantFields, DEBUG_TYPE, "VU Invariant Fields", false,
                false)

FunctionPass *llvm::createVUInvariantFieldsPass() {
  return new VUInvariantFields();
}

/// The index of the dest mask operand of an upper instruction that writes a
/// VF value, or 0. Its first immediate operand is the mask (an ACC input can
/// come before it). Pseudos have other immediates.
static unsigned getMaskIdx(const MachineInstr &MI) {
  if (MI.isPseudo() || MI.getDesc().TSFlags & 1 /* IsLower */ ||
      MI.getNumExplicitDefs() != 1 || !MI.getOperand(0).isReg() ||
      MI.hasUnmodeledSideEffects() || MI.mayLoadOrStore())
    return 0;
  for (unsigned I = 1, E = MI.getNumExplicitOperands(); I != E; ++I) {
    const MachineOperand &MO = MI.getOperand(I);
    if (MO.isReg() && MO.isTied())
      return 0;
    if (MO.isImm())
      return I;
  }
  return 0;
}

bool VUInvariantFields::fold(MachineInstr &Merge) {
  MachineBasicBlock &MBB = *Merge.getParent();
  Register D = Merge.getOperand(0).getReg();
  Register Src = Merge.getOperand(1).getReg();
  Register S = Merge.getOperand(2).getReg();
  unsigned M = Merge.getOperand(3).getImm();
  // The value from outside of the loop, and the one computed in it with the
  // fields it gives.
  auto Outside = [&](Register R) {
    MachineInstr *Def = R.isVirtual() ? MRI->getVRegDef(R) : nullptr;
    return Def && Def->getParent() != &MBB;
  };
  Register Inv, Var;
  unsigned VarMask;
  if (Outside(S) && !Outside(Src)) {
    Inv = S, Var = Src, VarMask = ~M & 0xF;
  } else if (Outside(Src) && !Outside(S)) {
    Inv = Src, Var = S, VarMask = M;
  } else {
    return false;
  }
  if (!Var.isVirtual() || !MRI->hasOneNonDBGUse(Var))
    return false;
  for (const MachineInstr &Use : MRI->use_nodbg_instructions(D))
    if (!Use.mayStore() || Use.getParent() != &MBB)
      return false;
  MachineInstr *P = MRI->getVRegDef(Var);
  unsigned MaskIdx = P ? getMaskIdx(*P) : 0;
  if (!MaskIdx || P->getParent() != &MBB ||
      (P->getOperand(MaskIdx).getImm() & VarMask) != VarMask)
    return false;

  LLVM_DEBUG(dbgs() << "Fields of " << printReg(Inv)
                    << " before the loop: " << Merge);
  const TargetRegisterClass *RC = MRI->getRegClass(D);
  Register R = MRI->createVirtualRegister(RC);
  Register New = MRI->createVirtualRegister(RC);
  auto Phi = BuildMI(MBB, MBB.getFirstNonPHI(), Merge.getDebugLoc(),
                     TII->get(TargetOpcode::PHI), R);
  for (MachineBasicBlock *Pred : MBB.predecessors())
    Phi.addReg(Pred == &MBB ? New : Inv).addMBB(Pred);
  MRI->constrainRegClass(Inv, RC);

  P->getOperand(0).setReg(New);
  P->getOperand(MaskIdx).setImm(VarMask);
  P->addOperand(MachineOperand::CreateReg(R, /*isDef=*/false,
                                          /*isImp=*/true));
  P->tieOperands(0, P->getNumOperands() - 1);
  MRI->replaceRegWith(D, New);
  Merge.eraseFromParent();
  ++NumFolded;
  return true;
}

bool VUInvariantFields::runOnMachineFunction(MachineFunction &MF) {
  if (!Enable || skipFunction(MF.getFunction()))
    return false;
  MRI = &MF.getRegInfo();
  TII = MF.getSubtarget().getInstrInfo();
  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    if (!MBB.isSuccessor(&MBB))
      continue;
    for (MachineInstr &MI : make_early_inc_range(MBB))
      if (MI.getOpcode() == VU::MERGE_FIELDS)
        Changed |= fold(MI);
  }
  return Changed;
}
