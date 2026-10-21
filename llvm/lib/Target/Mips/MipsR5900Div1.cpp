//===- MipsR5900Div1.cpp - Use the R5900 pipe 1 divider -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The R5900 has a second multiply/divide unit with its own HI1/LO1
// accumulator. DIV1 takes as long as DIV (37 cycles), but the two run in
// parallel, and neither blocks the loads, stores or ALU instructions
// (measured). This pass moves a divide that does not depend on the previous
// divide in the block to the pipe 1 accumulator, so the schedulers can overlap
// the two:
//
//   div   $zero, $4, $5         div   $zero, $4, $5
//   mflo  $2               ->   div1  $zero, $6, $7
//   div   $zero, $6, $7         mflo  $2
//   mflo  $3                    mflo1 $3
//
// The accumulator classes have a single register each, and the accumulator
// DAG mutation keeps the live ranges in each class in order, so alternating
// the classes lets two divides be in flight.
//
//===----------------------------------------------------------------------===//

#include "Mips.h"
#include "MipsInstrInfo.h"
#include "MipsSubtarget.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/CommandLine.h"

#define DEBUG_TYPE "mips-r5900-div1"

using namespace llvm;

STATISTIC(NumDiv1, "Number of divides moved to the pipe 1 divider");

static cl::opt<bool>
    DisableR5900Div1("disable-mips-r5900-div1", cl::init(false), cl::Hidden,
                     cl::desc("Do not use the R5900 pipe 1 divider for a "
                              "divide that is independent of the previous "
                              "one"));

namespace {

class MipsR5900Div1 : public MachineFunctionPass {
public:
  static char ID;

  MipsR5900Div1() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "R5900 pipe 1 divider"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  const MipsInstrInfo *TII = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  MachineRegisterInfo *MRI = nullptr;

  bool usesAC1(const MachineInstr &MI) const;
  bool canMove(MachineInstr &Div) const;
  void move(MachineInstr &Div);
};

} // end anonymous namespace

char MipsR5900Div1::ID = 0;

INITIALIZE_PASS(MipsR5900Div1, DEBUG_TYPE, "R5900 pipe 1 divider", false, false)

static bool isDiv(const MachineInstr &MI) {
  return MI.getOpcode() == Mips::PseudoSDIV ||
         MI.getOpcode() == Mips::PseudoUDIV;
}

/// Return true if \p MI reads or writes HI1/LO1 directly, such as a 128-bit
/// MMI multiply, or clobbers them, such as a call.
bool MipsR5900Div1::usesAC1(const MachineInstr &MI) const {
  for (const MachineOperand &MO : MI.operands()) {
    if (MO.isRegMask() && MO.clobbersPhysReg(Mips::AC1))
      return true;
    if (MO.isReg() && MO.getReg().isPhysical() &&
        TRI->regsOverlap(MO.getReg(), Mips::AC1))
      return true;
  }
  return false;
}

/// Return true if the result of \p Div is only read by MFHI/MFLO in its block,
/// with nothing in between that uses HI1/LO1.
bool MipsR5900Div1::canMove(MachineInstr &Div) const {
  Register Acc = Div.getOperand(0).getReg();
  MachineBasicBlock *MBB = Div.getParent();
  SmallPtrSet<const MachineInstr *, 4> Readers;
  for (MachineInstr &UseMI : MRI->use_nodbg_instructions(Acc)) {
    if (UseMI.getParent() != MBB || (UseMI.getOpcode() != Mips::PseudoMFHI &&
                                     UseMI.getOpcode() != Mips::PseudoMFLO))
      return false;
    Readers.insert(&UseMI);
  }
  for (MachineInstr &MI :
       make_range(std::next(Div.getIterator()), MBB->instr_end())) {
    if (Readers.empty())
      return true;
    if (Readers.erase(&MI))
      continue;
    if (usesAC1(MI))
      return false;
  }
  return Readers.empty();
}

void MipsR5900Div1::move(MachineInstr &Div) {
  MachineBasicBlock &MBB = *Div.getParent();
  Register OldAcc = Div.getOperand(0).getReg();
  Register Acc = MRI->createVirtualRegister(&Mips::ACC64_1RegClass);
  unsigned Opc = Div.getOpcode() == Mips::PseudoSDIV ? Mips::PseudoSDIV1
                                                     : Mips::PseudoUDIV1;
  BuildMI(MBB, Div, Div.getDebugLoc(), TII->get(Opc), Acc)
      .add(Div.getOperand(1))
      .add(Div.getOperand(2));
  for (MachineInstr &UseMI :
       make_early_inc_range(MRI->use_nodbg_instructions(OldAcc))) {
    unsigned UseOpc = UseMI.getOpcode() == Mips::PseudoMFHI ? Mips::PseudoMFHI1
                                                            : Mips::PseudoMFLO1;
    BuildMI(MBB, UseMI, UseMI.getDebugLoc(), TII->get(UseOpc),
            UseMI.getOperand(0).getReg())
        .addReg(Acc);
    UseMI.eraseFromParent();
  }
  Div.eraseFromParent();
  ++NumDiv1;
}

bool MipsR5900Div1::runOnMachineFunction(MachineFunction &MF) {
  const MipsSubtarget &STI = MF.getSubtarget<MipsSubtarget>();
  if (DisableR5900Div1 || !STI.isR5900() || skipFunction(MF.getFunction()))
    return false;

  TII = STI.getInstrInfo();
  TRI = STI.getRegisterInfo();
  MRI = &MF.getRegInfo();

  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    // The divides to move, and the registers that depend on the result of
    // the previous divide.
    SmallVector<MachineInstr *, 4> ToMove;
    DenseSet<Register> Dependent;
    bool HavePrev = false, PrevMoved = false;
    for (MachineInstr &MI : MBB) {
      bool IsDependent = any_of(MI.uses(), [&](const MachineOperand &MO) {
        return MO.isReg() && Dependent.contains(MO.getReg());
      });
      if (isDiv(MI)) {
        // Alternate the dividers for divides that are independent.
        PrevMoved = HavePrev && !PrevMoved && !IsDependent && canMove(MI);
        if (PrevMoved)
          ToMove.push_back(&MI);
        HavePrev = true;
        Dependent.clear();
        Dependent.insert(MI.getOperand(0).getReg());
        continue;
      }
      if (IsDependent)
        for (const MachineOperand &MO : MI.all_defs())
          if (MO.getReg().isVirtual())
            Dependent.insert(MO.getReg());
    }
    for (MachineInstr *Div : ToMove)
      move(*Div);
    Changed |= !ToMove.empty();
  }
  return Changed;
}

FunctionPass *llvm::createMipsR5900Div1Pass() { return new MipsR5900Div1(); }
