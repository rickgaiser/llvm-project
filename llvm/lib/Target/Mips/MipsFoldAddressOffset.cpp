//===- MipsFoldAddressOffset.cpp - Fold address offsets into memory ops ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Fold an add of an immediate to a register into the offsets of the loads and
// stores that use the sum, or the sum of it and an index, as their base:
//
//   addiu $1, $30, 72
//   lw    $1, 0($1)      ->   lw $1, 72($30)
//
//   addiu $3, $30, 120
//   addu  $1, $3, $1     ->   addu $1, $30, $1
//   lw    $1, 0($1)           lw   $1, 120($1)
//
// The register allocator rematerializes such an add before each use of an
// address that was computed once for the whole function, such as the fields
// of a structure pointer in a large interpreter loop, where CodeGenPrepare did
// not sink the address to its uses. The base register is live at the add, so
// using it in the loads and stores does not need another register.
//
//===----------------------------------------------------------------------===//

#include "Mips.h"
#include "MipsInstrInfo.h"
#include "MipsSubtarget.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

#define DEBUG_TYPE "mips-fold-address-offset"

STATISTIC(NumFolded, "Number of address adds folded into memory offsets");

static cl::opt<bool>
    DisableFold("disable-mips-r5900-fold-address-offset", cl::init(false),
                cl::Hidden,
                cl::desc("Do not fold address adds into the offsets of R5900 "
                         "loads and stores"));

namespace {

class MipsFoldAddressOffset : public MachineFunctionPass {
public:
  static char ID;
  MipsFoldAddressOffset() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "Mips Fold Address Offsets"; }

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoVRegs();
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  const TargetRegisterInfo *TRI = nullptr;
  bool collectUses(Register Reg, MachineBasicBlock::iterator From, int64_t Imm,
                   Register Keep, SmallVectorImpl<MachineInstr *> &Mems,
                   SmallVectorImpl<MachineInstr *> *Sums,
                   SmallVectorImpl<MachineInstr *> *SumMems,
                   SmallVectorImpl<MachineInstr *> &DebugUses);
  bool foldAdd(MachineInstr &Add);
};

} // namespace

char MipsFoldAddressOffset::ID = 0;

INITIALIZE_PASS(MipsFoldAddressOffset, DEBUG_TYPE, "Mips Fold Address Offsets",
                false, false)

/// Whether MI is a load or store with a register base and a 16-bit offset in
/// operands 1 and 2.
static bool isBasePlusOffset(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
  case Mips::LB:
  case Mips::LBu:
  case Mips::LH:
  case Mips::LHu:
  case Mips::LW:
  case Mips::SB:
  case Mips::SH:
  case Mips::SW:
  case Mips::LB64:
  case Mips::LBu64:
  case Mips::LH64:
  case Mips::LHu64:
  case Mips::LW64:
  case Mips::LWu:
  case Mips::LD:
  case Mips::SB64:
  case Mips::SH64:
  case Mips::SW64:
  case Mips::SD:
  case Mips::LWC1:
  case Mips::SWC1:
  case Mips::LQ:
  case Mips::SQ:
  case Mips::LQC2:
  case Mips::SQC2:
    return MI.getOperand(1).isReg() && MI.getOperand(2).isImm();
  default:
    return false;
  }
}

/// Collect the uses of Reg after From until it is redefined: loads and stores
/// with Reg only as their base and an offset that still fits with Imm added,
/// and, if Sums is given, register adds with Reg as one operand whose sums are
/// used the same way, by the loads and stores in SumMems. A sum may be in Reg
/// itself, so its loads and stores are kept apart from those of Reg. Keep must
/// not change while Reg is live, and Reg must not be live out of the block.
bool MipsFoldAddressOffset::collectUses(
    Register Reg, MachineBasicBlock::iterator From, int64_t Imm, Register Keep,
    SmallVectorImpl<MachineInstr *> &Mems,
    SmallVectorImpl<MachineInstr *> *Sums,
    SmallVectorImpl<MachineInstr *> *SumMems,
    SmallVectorImpl<MachineInstr *> &DebugUses) {
  MachineBasicBlock &MBB = *From->getParent();
  bool Ended = false;
  for (MachineInstr &MI : make_range(std::next(From), MBB.end())) {
    if (MI.isDebugInstr()) {
      if (MI.readsRegister(Reg, TRI))
        DebugUses.push_back(&MI);
      continue;
    }
    if (MI.readsRegister(Reg, TRI)) {
      // Reg may only be read once, apart from being redefined.
      unsigned Reads = 0;
      for (const MachineOperand &MO : MI.operands())
        if (MO.isReg() && MO.isUse() && TRI->regsOverlap(MO.getReg(), Reg))
          ++Reads;
      if (Reads != 1)
        return false;
      if (isBasePlusOffset(MI) && MI.getOperand(1).getReg() == Reg &&
          isInt<16>(MI.getOperand(2).getImm() + Imm)) {
        Mems.push_back(&MI);
      } else if (Sums &&
                 (MI.getOpcode() == Mips::ADDu ||
                  MI.getOpcode() == Mips::DADDu) &&
                 MI.getOperand(0).getReg() != Keep &&
                 collectUses(MI.getOperand(0).getReg(), MI.getIterator(), Imm,
                             Register(), *SumMems, nullptr, nullptr,
                             DebugUses)) {
        Sums->push_back(&MI);
      } else {
        return false;
      }
    }
    if (MI.modifiesRegister(Reg, TRI)) {
      Ended = true;
      break;
    }
    if (Keep && MI.modifiesRegister(Keep, TRI))
      return false;
  }
  if (!Ended)
    for (MachineBasicBlock *Succ : MBB.successors())
      for (MCRegAliasIterator AI(Reg, TRI, true); AI.isValid(); ++AI)
        if (Succ->isLiveIn(*AI))
          return false;
  return true;
}

bool MipsFoldAddressOffset::foldAdd(MachineInstr &Add) {
  Register Dst = Add.getOperand(0).getReg();
  Register Src = Add.getOperand(1).getReg();
  if (!Add.getOperand(2).isImm() || Dst == Src || Dst == Mips::R0 ||
      Dst == Mips::R0_64)
    return false;
  int64_t Imm = Add.getOperand(2).getImm();

  // The loads and stores with Dst, or a sum of it and another register, as
  // their base, with Src unchanged while Dst is live.
  SmallVector<MachineInstr *, 4> Mems, Sums, SumMems, DebugUses;
  if (!collectUses(Dst, Add.getIterator(), Imm, Src, Mems, &Sums, &SumMems,
                   DebugUses) ||
      (Mems.empty() && Sums.empty()))
    return false;

  // The instructions that read Src instead of Dst.
  SmallPtrSet<MachineInstr *, 8> Rewritten;
  for (MachineInstr *MI : Mems) {
    MI->getOperand(1).setReg(Src);
    MI->getOperand(2).setImm(MI->getOperand(2).getImm() + Imm);
    Rewritten.insert(MI);
  }
  for (MachineInstr *MI : SumMems)
    MI->getOperand(2).setImm(MI->getOperand(2).getImm() + Imm);
  for (MachineInstr *MI : Sums) {
    for (MachineOperand &MO : MI->uses())
      if (MO.isReg() && MO.getReg() == Dst)
        MO.setReg(Src);
    Rewritten.insert(MI);
  }
  // The sum is no longer computed.
  for (MachineInstr *MI : DebugUses)
    if (MI->isDebugValue())
      MI->setDebugValueUndef();
  // Src is now read up to the last of them.
  unsigned Left = Rewritten.size();
  for (MachineInstr &MI :
       make_range(MachineBasicBlock::iterator(Add), Add.getParent()->end())) {
    MI.clearRegisterKills(Src, TRI);
    if (Rewritten.count(&MI) && --Left == 0)
      break;
  }
  Add.eraseFromParent();
  ++NumFolded;
  return true;
}

bool MipsFoldAddressOffset::runOnMachineFunction(MachineFunction &MF) {
  const MipsSubtarget &STI = MF.getSubtarget<MipsSubtarget>();
  if (DisableFold || !STI.isR5900() || skipFunction(MF.getFunction()))
    return false;
  TRI = STI.getRegisterInfo();

  bool Changed = false;
  for (MachineBasicBlock &MBB : MF)
    for (MachineInstr &MI : make_early_inc_range(MBB))
      if (MI.getOpcode() == Mips::ADDiu || MI.getOpcode() == Mips::DADDiu)
        Changed |= foldAdd(MI);
  return Changed;
}

FunctionPass *llvm::createMipsFoldAddressOffsetPass() {
  return new MipsFoldAddressOffset();
}
