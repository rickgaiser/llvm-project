//===-- VUPackConstants.cpp - Share VF registers between constants --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A float constant is LOADF32 (LOI c; ADDi.x d, vf0, i): a VF register for its
// x field. Constants that a block makes for other blocks, such as those that
// a loop preheader holds for the loop, share registers, up to four each, one
// per field, when the other blocks read them by a broadcast field. The
// registers that this frees are left for the values of the loop, such as
// those of a pipelined one:
//   %a = LOADF32 0.5; %b = LOADF32 3.0; ... (loop) MULx %d, %v, %a; ADDx %e, %d, %b
//   ->
//   %a = LOADF32 0.5; %b = LOADF32 3.0; %p = INSERT_FIELD %a, %b, y, x
//   ... (loop) MULx %d, %v, %p; ADDy %e, %d, %p
// The uses that can read another field are the broadcast forms of the FMAC
// instructions (ft), SPLAT and INSERT_FIELD (the inserted value).
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUMCTargetDesc.h"
#include "VU.h"
#include "VUSubtarget.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"

using namespace llvm;

#define DEBUG_TYPE "vu-pack-constants"

namespace {
class VUPackConstants : public MachineFunctionPass {
public:
  static char ID;
  VUPackConstants() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "VU Pack Constants"; }
  bool runOnMachineFunction(MachineFunction &MF) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};
} // namespace

char VUPackConstants::ID = 0;

INITIALIZE_PASS(VUPackConstants, DEBUG_TYPE, "VU Pack Constants", false, false)

FunctionPass *llvm::createVUPackConstantsPass() { return new VUPackConstants(); }

/// The broadcast forms of the FMAC instructions, by field x, y, z, w.
static constexpr unsigned BroadcastForms[][4] = {
    {VU::ADDx, VU::ADDy, VU::ADDz, VU::ADDw},
    {VU::SUBx, VU::SUBy, VU::SUBz, VU::SUBw},
    {VU::MULx, VU::MULy, VU::MULz, VU::MULw},
    {VU::MADDx, VU::MADDy, VU::MADDz, VU::MADDw},
    {VU::MSUBx, VU::MSUBy, VU::MSUBz, VU::MSUBw},
    {VU::MAXx, VU::MAXy, VU::MAXz, VU::MAXw},
    {VU::MINIx, VU::MINIy, VU::MINIz, VU::MINIw},
    {VU::ADDAx, VU::ADDAy, VU::ADDAz, VU::ADDAw},
    {VU::SUBAx, VU::SUBAy, VU::SUBAz, VU::SUBAw},
    {VU::MULAx, VU::MULAy, VU::MULAz, VU::MULAw},
    {VU::MADDAx, VU::MADDAy, VU::MADDAz, VU::MADDAw},
    {VU::MSUBAx, VU::MSUBAy, VU::MSUBAz, VU::MSUBAw},
};

/// The opcode that reads field \p Field instead of x, or 0 if \p Opc is not
/// a broadcast form of field x.
static unsigned getFieldForm(unsigned Opc, unsigned Field) {
  for (const auto &Forms : BroadcastForms)
    if (Forms[0] == Opc)
      return Forms[Field];
  return 0;
}

/// Returns true if \p MO, a use of a constant, can read another field of a
/// register instead.
static bool canReadField(const MachineOperand &MO) {
  const MachineInstr &MI = *MO.getParent();
  unsigned Idx = MI.getOperandNo(&MO);
  switch (MI.getOpcode()) {
  case VU::SPLAT:
    return Idx == 1 && MI.getOperand(2).getImm() == 0;
  case VU::INSERT_FIELD:
    return Idx == 2 && MI.getOperand(4).getImm() == 0;
  default:
    // ft is the last explicit operand of a broadcast form.
    return getFieldForm(MI.getOpcode(), 0) &&
           Idx == MI.getNumExplicitOperands() - 1;
  }
}

/// Makes \p MO read field \p Field of \p Reg.
static void readField(MachineOperand &MO, Register Reg, unsigned Field,
                      const TargetInstrInfo &TII) {
  MachineInstr &MI = *MO.getParent();
  MO.setReg(Reg);
  switch (MI.getOpcode()) {
  case VU::SPLAT:
    MI.getOperand(2).setImm(Field);
    return;
  case VU::INSERT_FIELD:
    MI.getOperand(4).setImm(Field);
    return;
  default:
    MI.setDesc(TII.get(getFieldForm(MI.getOpcode(), Field)));
    return;
  }
}

bool VUPackConstants::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;
  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
  MachineRegisterInfo &MRI = MF.getRegInfo();
  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    // The constants of the block that other blocks, such as a loop, use, and
    // where all those uses can read another field. The shared register is
    // made at the end of the block, and only those uses read it.
    auto OutsideUses = [&](Register R) {
      SmallVector<MachineOperand *, 8> Uses;
      for (MachineOperand &MO : MRI.use_nodbg_operands(R))
        if (MO.getParent()->getParent() != &MBB)
          Uses.push_back(&MO);
      return Uses;
    };
    SmallVector<MachineInstr *, 8> Consts;
    for (MachineInstr &MI : MBB) {
      if (MI.getOpcode() != VU::LOADF32 ||
          !MI.getOperand(0).getReg().isVirtual())
        continue;
      auto Uses = OutsideUses(MI.getOperand(0).getReg());
      if (!Uses.empty() &&
          all_of(Uses, [](MachineOperand *MO) { return canReadField(*MO); }))
        Consts.push_back(&MI);
    }
    for (unsigned G = 0; G + 1 < Consts.size(); G += 4) {
      ArrayRef<MachineInstr *> Group =
          ArrayRef(Consts).slice(G, std::min<size_t>(4, Consts.size() - G));
      // Field 0 from the first constant, the others inserted.
      auto InsertPt = MBB.getFirstTerminator();
      Register Shared = Group[0]->getOperand(0).getReg();
      for (unsigned K = 1; K != Group.size(); ++K) {
        Register NewShared = MRI.createVirtualRegister(&VU::VFRegClass);
        BuildMI(MBB, InsertPt, Group[K]->getDebugLoc(),
                TII.get(VU::INSERT_FIELD), NewShared)
            .addReg(Shared)
            .addReg(Group[K]->getOperand(0).getReg())
            .addImm(K)
            .addImm(0);
        Shared = NewShared;
      }
      for (unsigned K = 0; K != Group.size(); ++K)
        for (MachineOperand *MO : OutsideUses(Group[K]->getOperand(0).getReg()))
          readField(*MO, Shared, K, TII);
      LLVM_DEBUG(dbgs() << "Packed " << Group.size() << " constants into "
                        << printReg(Shared) << '\n');
      Changed = true;
    }
  }
  return Changed;
}
