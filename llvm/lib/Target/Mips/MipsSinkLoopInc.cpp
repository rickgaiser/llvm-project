//===- MipsSinkLoopInc.cpp - Sink loop increments below the last use ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Instruction selection puts the increment of a loop's pointer in front of the
// loads and stores that still read the old value:
//
//   %next = ADDiu %cur, 4
//   %v = LW %cur, 0
//
// The two values overlap, so they get a register each and the loop ends with a
// move from one to the other: on the single-issue R3000A a cycle per iteration.
// This pass moves the increment down to the last load that reads %cur, where it
// fills the delay slot of that load, and makes the stores behind it read the
// new value, with the offset changed:
//
//   %v = LW %cur, 0
//   %next = ADDiu %cur, 4
//   SW %v, %next, -4
//
// The two values then coalesce, and the store is free to go into a branch delay
// slot. A reader that cannot be changed takes the increment below it.
//
//===----------------------------------------------------------------------===//

#include "Mips.h"
#include "MipsSubtarget.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

#define DEBUG_TYPE "mips-sink-loop-inc"

namespace {

class MipsSinkLoopInc : public MachineFunctionPass {
public:
  static char ID;

  MipsSinkLoopInc() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "Mips sink loop increments"; }

  bool runOnMachineFunction(MachineFunction &MF) override;
};

} // namespace

INITIALIZE_PASS(MipsSinkLoopInc, DEBUG_TYPE, "Mips sink loop increments", false,
                false)

char MipsSinkLoopInc::ID = 0;

FunctionPass *llvm::createMipsSinkLoopIncPass() {
  return new MipsSinkLoopInc();
}

/// Whether \p Next is the value of a PHI that defines \p Cur.
static bool feedsPhiOf(const MachineRegisterInfo &MRI, Register Next,
                       Register Cur) {
  const MachineInstr *Def = MRI.getVRegDef(Cur);
  if (!Def || !Def->isPHI())
    return false;
  for (unsigned I = 1, E = Def->getNumOperands(); I < E; I += 2)
    if (Def->getOperand(I).getReg() == Next)
      return true;
  return false;
}

/// Whether \p MI reads \p Cur only as the base of a memory access, and can do
/// so from a register \p Imm higher.
static bool canRebase(const MachineInstr &MI, Register Cur, int64_t Imm) {
  switch (MI.getOpcode()) {
  case Mips::LB:
  case Mips::LBu:
  case Mips::LH:
  case Mips::LHu:
  case Mips::LW:
  case Mips::LWL:
  case Mips::LWR:
  case Mips::SB:
  case Mips::SH:
  case Mips::SW:
  case Mips::SWL:
  case Mips::SWR:
    break;
  default:
    return false;
  }
  const MachineOperand &Base = MI.getOperand(1);
  const MachineOperand &Off = MI.getOperand(2);
  if (!Base.isReg() || Base.getReg() != Cur || !Off.isImm() ||
      !isInt<16>(Off.getImm() - Imm))
    return false;
  // The value of a store, or the merged value of LWL and LWR, must not be Cur.
  for (unsigned I = 0, E = MI.getNumOperands(); I != E; ++I)
    if (I != 1 && MI.getOperand(I).isReg() && MI.getOperand(I).getReg() == Cur)
      return false;
  return true;
}

/// Give \p Inc the place where it costs nothing: behind the last load through
/// its source, which leaves it in that load's delay slot, or, with only stores
/// through it, in front of them, where the scheduler can use it to fill a delay
/// slot. The readers behind it are changed to its result.
static bool moveInc(MachineInstr &Inc) {
  MachineBasicBlock &MBB = *Inc.getParent();
  Register Next = Inc.getOperand(0).getReg();
  Register Cur = Inc.getOperand(1).getReg();
  int64_t Imm = Inc.getOperand(2).getImm();

  // It cannot go past the first reader of its result, a call or the
  // terminators.
  MachineBasicBlock::iterator Limit = std::next(Inc.getIterator());
  for (MachineBasicBlock::iterator E = MBB.end(); Limit != E; ++Limit)
    if (!Limit->isDebugInstr() && (Limit->isTerminator() || Limit->isCall() ||
                                   Limit->readsVirtualRegister(Next)))
      break;

  // Find the last reader that keeps the old value, and the first of the others.
  MachineBasicBlock::iterator Begin = MBB.getFirstNonPHI();
  MachineBasicBlock::iterator Keep = MBB.end(), First = MBB.end();
  for (MachineBasicBlock::iterator I = Begin; I != Limit; ++I) {
    if (&*I == &Inc || I->isDebugInstr() || !I->readsVirtualRegister(Cur))
      continue;
    if (I->mayLoad() || !canRebase(*I, Cur, Imm)) {
      Keep = I;
      First = MBB.end();
    } else if (First == MBB.end()) {
      First = I;
    }
  }

  // Where it goes, and from where its readers change.
  MachineBasicBlock::iterator Pos;
  if (Keep != MBB.end())
    Pos = std::next(Keep);
  else if (First != MBB.end())
    Pos = First;
  else
    return false;

  // All readers behind that take the result, also those behind the limit.
  SmallVector<MachineInstr *, 4> Readers;
  for (MachineBasicBlock::iterator I = Pos, E = MBB.end(); I != E; ++I) {
    if (&*I == &Inc || I->isDebugInstr() || !I->readsVirtualRegister(Cur))
      continue;
    if (!canRebase(*I, Cur, Imm))
      return false;
    Readers.push_back(&*I);
  }

  for (MachineInstr *MI : Readers) {
    MI->getOperand(1).setReg(Next);
    MI->getOperand(1).setIsKill(false);
    MI->getOperand(2).setImm(MI->getOperand(2).getImm() - Imm);
  }
  if (Pos != Inc.getIterator())
    MBB.splice(Pos, &MBB, Inc.getIterator());
  return true;
}

bool MipsSinkLoopInc::runOnMachineFunction(MachineFunction &MF) {
  if (!MF.getSubtarget<MipsSubtarget>().isR3000A())
    return false;

  MachineRegisterInfo &MRI = MF.getRegInfo();
  bool Changed = false;

  for (MachineBasicBlock &MBB : MF) {
    SmallVector<MachineInstr *, 4> Incs;
    for (MachineInstr &MI : MBB)
      if (MI.getOpcode() == Mips::ADDiu && MI.getOperand(1).isReg() &&
          MI.getOperand(2).isImm() && MI.getOperand(0).getReg().isVirtual() &&
          MI.getOperand(1).getReg().isVirtual() &&
          feedsPhiOf(MRI, MI.getOperand(0).getReg(), MI.getOperand(1).getReg()))
        Incs.push_back(&MI);

    for (MachineInstr *Inc : Incs)
      Changed |= moveInc(*Inc);
  }
  return Changed;
}
