//===- MipsSplitHi16.cpp - Give every %hi use its own lui -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Instruction selection shares one "lui $r, %hi(sym)" between every load and
// store of the same symbol, which leaves more R_MIPS_LO16 than R_MIPS_HI16
// relocations in the object. The PS2 IOP module format (IRX1) pairs each HI16
// with the single LO16 that follows it, so a shared lui cannot be relocated
// correctly. With -mips-split-hi16 this pass clones the lui in front of each
// of its users, so every HI16 has exactly one LO16. Each lui also gets an
// implicit use of $zero so that MachineLateInstrsCleanup does not delete a
// lui as a duplicate once register allocation gives two of them the same
// register. A second pass after register allocation moves each lui down to
// the instruction that reads it, so the HI16 and LO16 relocations end up next
// to each other.
//
//===----------------------------------------------------------------------===//

#include "Mips.h"
#include "MCTargetDesc/MipsBaseInfo.h"
#include "MipsInstrInfo.h"
#include "MipsSubtarget.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/Support/CommandLine.h"

#define DEBUG_TYPE "mips-split-hi16"

using namespace llvm;

namespace llvm {
extern cl::opt<bool> MipsSplitHi16Opt;
} // namespace llvm

namespace {

class MipsSplitHi16 : public MachineFunctionPass {
public:
  MipsSplitHi16() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "Mips split HI16"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

  static char ID;
};

} // namespace

INITIALIZE_PASS(MipsSplitHi16, DEBUG_TYPE, "Mips split HI16", false, false)

char MipsSplitHi16::ID = 0;

/// Fewest users of one lui for which a shared address pays off: the shared
/// form costs lui + addiu + one instruction per use, the split form two per
/// use.
static constexpr unsigned SharedGlobalBaseMinUses = 3;

/// If every user of the lui is a plain "%lo(sym+offset)" access to the same
/// global, replaces the users' %lo operands with the offsets and gives them one
/// base register holding the address of the symbol, built with an adjacent
/// lui and addiu pair. Returns true when it did.
static bool shareGlobalBase(MachineInstr &Lui, MachineRegisterInfo &MRI,
                            const TargetInstrInfo &TII) {
  const MachineOperand &Hi = Lui.getOperand(1);
  if (!Hi.isGlobal() || Hi.getOffset() != 0)
    return false;

  Register Def = Lui.getOperand(0).getReg();
  SmallVector<std::pair<MachineOperand *, MachineOperand *>, 8> Users;
  for (MachineOperand &MO : MRI.use_nodbg_operands(Def)) {
    MachineInstr *User = MO.getParent();
    unsigned Idx = User->getOperandNo(&MO);
    if (User->isPHI() || User->isCopy() || Idx + 1 >= User->getNumOperands())
      return false;
    MachineOperand &Lo = User->getOperand(Idx + 1);
    if (!Lo.isGlobal() || Lo.getGlobal() != Hi.getGlobal() ||
        Lo.getTargetFlags() != MipsII::MO_ABS_LO || !isInt<16>(Lo.getOffset()))
      return false;
    Users.push_back({&MO, &Lo});
  }
  if (Users.size() < SharedGlobalBaseMinUses)
    return false;

  MachineBasicBlock &MBB = *Lui.getParent();
  Register Base = MRI.createVirtualRegister(MRI.getRegClass(Def));
  BuildMI(MBB, std::next(Lui.getIterator()), Lui.getDebugLoc(),
          TII.get(Mips::ADDiu), Base)
      .addReg(Def)
      .addGlobalAddress(Hi.getGlobal(), 0, MipsII::MO_ABS_LO);
  for (auto &[Use, Lo] : Users) {
    int64_t Offset = Lo->getOffset();
    Use->setReg(Base);
    Lo->ChangeToImmediate(Offset);
  }
  return true;
}

bool MipsSplitHi16::runOnMachineFunction(MachineFunction &MF) {
  if (!MipsSplitHi16Opt)
    return false;

  const MipsSubtarget &STI = MF.getSubtarget<MipsSubtarget>();

  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
  MachineRegisterInfo &MRI = MF.getRegInfo();
  bool Modified = false;

  SmallVector<MachineInstr *, 16> Luis;
  for (MachineBasicBlock &MBB : MF)
    for (MachineInstr &MI : MBB)
      if (MI.getOpcode() == Mips::LUi && MI.getNumOperands() == 2 &&
          MI.getOperand(1).getTargetFlags() == MipsII::MO_ABS_HI)
        Luis.push_back(&MI);

  for (MachineInstr *Lui : Luis) {
    Register Def = Lui->getOperand(0).getReg();
    MachineInstrBuilder(MF, Lui).addReg(Mips::R0, RegState::Implicit);
    Modified = true;
    if (STI.shareGlobalBase() && shareGlobalBase(*Lui, MRI, TII)) {
      // The new addiu reads the lui and pairs with it.
      continue;
    }
    SmallVector<MachineOperand *, 8> Uses;
    for (MachineOperand &MO : MRI.use_nodbg_operands(Def))
      Uses.push_back(&MO);
    if (Uses.empty())
      continue;

    // Every user gets its own lui in front of it, so a lui hoisted away from
    // its user does not leave a HI16 without a nearby LO16.
    for (MachineOperand *Use : Uses) {
      MachineInstr *User = Use->getParent();
      if (User->isPHI() || User->isCopy())
        continue;
      Register NewReg = MRI.createVirtualRegister(MRI.getRegClass(Def));
      BuildMI(*User->getParent(), User, User->getDebugLoc(),
              TII.get(Mips::LUi), NewReg)
          .add(Lui->getOperand(1))
          .addReg(Mips::R0, RegState::Implicit);
      Use->setReg(NewReg);
    }
    if (MRI.use_nodbg_empty(Def))
      Lui->eraseFromParent();
  }

  return Modified;
}

FunctionPass *llvm::createMipsSplitHi16Pass() { return new MipsSplitHi16(); }

namespace {

class MipsSinkHi16 : public MachineFunctionPass {
public:
  MipsSinkHi16() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "Mips sink HI16"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

  static char ID;
};

} // namespace

INITIALIZE_PASS(MipsSinkHi16, "mips-sink-hi16", "Mips sink HI16", false, false)

char MipsSinkHi16::ID = 0;

bool MipsSinkHi16::runOnMachineFunction(MachineFunction &MF) {
  if (!MipsSplitHi16Opt)
    return false;

  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  bool Modified = false;

  for (MachineBasicBlock &MBB : MF) {
    SmallVector<MachineInstr *, 16> Luis;
    for (MachineInstr &MI : MBB)
      if (MI.getOpcode() == Mips::LUi && MI.getNumOperands() >= 2 &&
          MI.getOperand(1).getTargetFlags() == MipsII::MO_ABS_HI)
        Luis.push_back(&MI);

    for (MachineInstr *Lui : Luis) {
      Register Reg = Lui->getOperand(0).getReg();
      MachineBasicBlock::iterator Reader = std::next(Lui->getIterator());
      for (; Reader != MBB.end(); ++Reader) {
        if (Reader->isDebugInstr())
          continue;
        if (Reader->readsRegister(Reg, TRI))
          break;
        if (Reader->modifiesRegister(Reg, TRI) || Reader->isCall()) {
          Reader = MBB.end();
          break;
        }
      }
      if (Reader == MBB.end() || Reader == std::next(Lui->getIterator()))
        continue;
      MBB.splice(Reader, &MBB, Lui->getIterator());
      Modified = true;
    }
  }

  return Modified;
}

FunctionPass *llvm::createMipsSinkHi16Pass() { return new MipsSinkHi16(); }
