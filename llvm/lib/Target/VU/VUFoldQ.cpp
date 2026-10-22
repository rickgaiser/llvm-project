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

char VUFoldQ::ID = 0;

INITIALIZE_PASS(VUFoldQ, DEBUG_TYPE, "VU Fold Q", false, false)

FunctionPass *llvm::createVUFoldQPass() { return new VUFoldQ(); }

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
