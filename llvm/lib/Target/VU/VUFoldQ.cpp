//===-- VUFoldQ.cpp - Read DIV and SQRT results directly from Q -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A division is selected as DIV followed by ADDq.x t, vf0, q, which moves Q
// into a VF register. Instructions that broadcast the x field of t are
// rewritten to read Q instead:
//   ADDq.x t, vf0, q; MULx.xyzw d, a, t   ->   MULq.xyzw d, a, q
// The ADDq is removed when it has no uses left. The rewritten instruction
// must be in the same block, without another write of Q in between: the two
// Q values would then be live at the same time.
//
//
// After register allocation, the reverse: a value of Q that the register
// allocator kept in a VF register is copied back with DIV q, v.x, vf0.w, which
// keeps the divider busy for 7 cycles. Its readers read v.x instead:
//   DIV q, v.x, vf0.w; MULq.xyzw d, a, q   ->   MULx.xyzw d, a, v
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUMCTargetDesc.h"
#include "VU.h"
#include "VUSubtarget.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"

using namespace llvm;

#define DEBUG_TYPE "vu-fold-q"

namespace {
class VUFoldQ : public MachineFunctionPass {
public:
  static char ID;
  VUFoldQ() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "VU Fold Q"; }
  bool runOnMachineFunction(MachineFunction &MF) override;
};
} // namespace

namespace {
class VUFoldQCopies : public MachineFunctionPass {
public:
  static char ID;
  VUFoldQCopies() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "VU Fold Q Copies"; }
  bool runOnMachineFunction(MachineFunction &MF) override;

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoVRegs();
  }
};
} // namespace

char VUFoldQ::ID = 0;
char VUFoldQCopies::ID = 0;

INITIALIZE_PASS(VUFoldQ, DEBUG_TYPE, "VU Fold Q", false, false)
INITIALIZE_PASS(VUFoldQCopies, "vu-fold-q-copies", "VU Fold Q Copies", false,
                false)

FunctionPass *llvm::createVUFoldQPass() { return new VUFoldQ(); }
FunctionPass *llvm::createVUFoldQCopiesPass() { return new VUFoldQCopies(); }

/// Returns the Q form of an instruction that broadcasts the x field of ft,
/// or 0.
static unsigned getQForm(unsigned Opc) {
  switch (Opc) {
  case VU::ADDx:
    return VU::ADDq;
  case VU::SUBx:
    return VU::SUBq;
  case VU::MULx:
    return VU::MULq;
  case VU::MADDx:
    return VU::MADDq;
  case VU::MSUBx:
    return VU::MSUBq;
  case VU::ADDAx:
    return VU::ADDAq;
  case VU::SUBAx:
    return VU::SUBAq;
  case VU::MULAx:
    return VU::MULAq;
  case VU::MADDAx:
    return VU::MADDAq;
  case VU::MSUBAx:
    return VU::MSUBAq;
  default:
    return 0;
  }
}

bool VUFoldQ::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;

  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
  MachineRegisterInfo &MRI = MF.getRegInfo();
  bool Changed = false;

  for (MachineBasicBlock &MBB : MF) {
    // ADDq.x t, vf0, q
    SmallVector<MachineInstr *, 4> Moves;
    for (MachineInstr &MI : MBB)
      if (MI.getOpcode() == VU::ADDq && MI.getOperand(1).getImm() == 8 &&
          MI.getOperand(2).getReg() == VU::VF0 &&
          MI.getOperand(0).getReg().isVirtual())
        Moves.push_back(&MI);

    for (MachineInstr *MovePtr : Moves) {
      MachineInstr &Move = *MovePtr;
      Register T = Move.getOperand(0).getReg();
      const MachineOperand &Q = Move.getOperand(3);

      for (MachineBasicBlock::iterator I = std::next(Move.getIterator()),
                                       E = MBB.end();
           I != E;) {
        MachineInstr &MI = *I++;
        unsigned QOpc = getQForm(MI.getOpcode());
        // ft is the last explicit operand.
        unsigned FtIdx = MI.getNumExplicitOperands() - 1;
        if (QOpc && MI.getOperand(FtIdx).getReg() == T) {
          MachineInstrBuilder MIB =
              BuildMI(MBB, MI, MI.getDebugLoc(), TII.get(QOpc));
          for (unsigned Op = 0; Op != FtIdx; ++Op)
            MIB.add(MI.getOperand(Op));
          MIB.addReg(Q.getReg());
          MRI.clearKillFlags(Q.getReg());
          MI.eraseFromParent();
          Changed = true;
          continue;
        }
        if (any_of(MI.all_defs(), [&](const MachineOperand &MO) {
              Register R = MO.getReg();
              return R == VU::Q ||
                     (R.isVirtual() && MRI.getRegClass(R) == &VU::QRRegClass);
            }))
          break;
      }

      if (MRI.use_nodbg_empty(T)) {
        Move.eraseFromParent();
        Changed = true;
      }
    }
  }
  return Changed;
}

/// Returns the form of a Q form that broadcasts the x field of ft, or 0.
static unsigned getBroadcastXForm(unsigned QOpc) {
  for (unsigned Opc : {VU::ADDx, VU::SUBx, VU::MULx, VU::MADDx, VU::MSUBx,
                       VU::ADDAx, VU::SUBAx, VU::MULAx, VU::MADDAx, VU::MSUBAx})
    if (getQForm(Opc) == QOpc)
      return Opc;
  return 0;
}

bool VUFoldQCopies::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;
  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
  const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();
  bool Changed = false;

  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &Copy : make_early_inc_range(MBB)) {
      // DIV q, v.x, vf0.w
      if (Copy.getOpcode() != VU::DIV || Copy.getOperand(2).getImm() != 0 ||
          Copy.getOperand(3).getReg() != VU::VF0 ||
          Copy.getOperand(4).getImm() != 3)
        continue;
      Register V = Copy.getOperand(1).getReg();

      // The readers of this value of Q, up to the next write of Q. All must
      // have a broadcast form and come before V changes; a reader may change
      // V itself, since it reads first.
      SmallVector<MachineInstr *, 4> Readers;
      bool Ok = true, Ends = false, VChanged = false;
      for (MachineInstr &MI :
           make_range(std::next(Copy.getIterator()), MBB.instr_end())) {
        if (MI.readsRegister(VU::Q, &TRI)) {
          if (VChanged || !getBroadcastXForm(MI.getOpcode())) {
            Ok = false;
            break;
          }
          Readers.push_back(&MI);
        }
        if (MI.modifiesRegister(VU::Q, &TRI)) {
          Ends = true;
          break;
        }
        VChanged |= MI.modifiesRegister(V, &TRI);
      }
      // Or in a successor.
      if (Ok && !Ends)
        for (MachineBasicBlock *Succ : MBB.successors())
          if (Succ->isLiveIn(VU::Q))
            Ok = false;
      if (!Ok)
        continue;

      for (MachineInstr *MI : Readers) {
        // The Q form has q in the place of ft.
        unsigned QIdx = MI->getNumExplicitOperands() - 1;
        MI->setDesc(TII.get(getBroadcastXForm(MI->getOpcode())));
        MI->getOperand(QIdx).setReg(V);
        MI->getOperand(QIdx).setIsKill(false);
      }
      LLVM_DEBUG(dbgs() << "Removed the copy " << Copy);
      Copy.eraseFromParent();
      Changed = true;
    }
  }
  return Changed;
}
