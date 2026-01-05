//===- MipsR5900VU0AccChain.cpp - R5900 VU0 ACC Chain Optimization --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass recognizes chains of VU0 broadcast multiply operations followed by
// vector adds and converts them to use the VU0 accumulator instructions.
//
// Pattern: result = VMULbcx(m0, v) + VMULbcy(m1, v) + VMULbcz(m2, v) + VMULbcw(m3, v)
// Becomes: VMULAbcx ACC, m0, v
//          VMADDAbcy ACC, m1, v
//          VMADDAbcz ACC, m2, v
//          VMADDbcw result, m3, v
//
// This enables efficient matrix-vector multiplication on the R5900 VU0 unit.
//
//===----------------------------------------------------------------------===//

#include "Mips.h"
#include "MipsInstrInfo.h"
#include "MipsSubtarget.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Target/TargetMachine.h"

#define DEBUG_TYPE "mips-r5900-vu0-acc-chain"

using namespace llvm;

STATISTIC(NumVU0ChainsOptimized, "Number of VU0 ACC chains optimized");
STATISTIC(NumVU0InstructionsSaved, "Number of VU0 instructions saved");

namespace {

class MipsR5900VU0AccChain : public MachineFunctionPass {
public:
  MipsR5900VU0AccChain() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return "R5900 VU0 Accumulator Chain Optimization";
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  static char ID;

private:
  const MipsInstrInfo *TII = nullptr;
  const MipsSubtarget *STI = nullptr;
  MachineRegisterInfo *MRI = nullptr;

  /// Find the instruction that defines Reg
  MachineInstr *findDef(Register Reg);

  /// Optimize VU0 broadcast multiply-add chains in a basic block
  bool optimizeBasicBlock(MachineBasicBlock &MBB);

  /// Check if an instruction is VADD (v4f32 vector add)
  /// Recognizes both assembly and ISel variants
  bool isVADD(const MachineInstr &MI) const {
    unsigned Opc = MI.getOpcode();
    return Opc == Mips::VADD || Opc == Mips::VADD_ISel;
  }

  /// Check if an instruction is a VU0 broadcast multiply (VMULbcx/y/z/w)
  /// Recognizes both assembly and ISel variants
  bool isVMULbc(const MachineInstr &MI) const {
    unsigned Opc = MI.getOpcode();
    return Opc == Mips::VMULbcx || Opc == Mips::VMULbcy ||
           Opc == Mips::VMULbcz || Opc == Mips::VMULbcw ||
           Opc == Mips::VMULbc_ISelx || Opc == Mips::VMULbc_ISely ||
           Opc == Mips::VMULbc_ISelz || Opc == Mips::VMULbc_ISelw;
  }

  /// Get the broadcast component (0=x, 1=y, 2=z, 3=w) from a VMULbc instruction
  unsigned getBroadcastComponent(const MachineInstr &MI) const {
    switch (MI.getOpcode()) {
    case Mips::VMULbcx:
    case Mips::VMULbc_ISelx: return 0;
    case Mips::VMULbcy:
    case Mips::VMULbc_ISely: return 1;
    case Mips::VMULbcz:
    case Mips::VMULbc_ISelz: return 2;
    case Mips::VMULbcw:
    case Mips::VMULbc_ISelw: return 3;
    default: llvm_unreachable("Not a VMULbc instruction");
    }
  }

  /// Get the VMULAbc opcode for a given broadcast component
  unsigned getVMULAbcOpcode(unsigned Component) const {
    switch (Component) {
    case 0: return Mips::VMULAbcx;
    case 1: return Mips::VMULAbcy;
    case 2: return Mips::VMULAbcz;
    case 3: return Mips::VMULAbcw;
    default: llvm_unreachable("Invalid broadcast component");
    }
  }

  /// Get the VMADDAbc opcode for a given broadcast component
  unsigned getVMADDAbcOpcode(unsigned Component) const {
    switch (Component) {
    case 0: return Mips::VMADDAbcx;
    case 1: return Mips::VMADDAbcy;
    case 2: return Mips::VMADDAbcz;
    case 3: return Mips::VMADDAbcw;
    default: llvm_unreachable("Invalid broadcast component");
    }
  }

  /// Get the VMADDbc opcode for a given broadcast component
  unsigned getVMADDbcOpcode(unsigned Component) const {
    switch (Component) {
    case 0: return Mips::VMADDbcx;
    case 1: return Mips::VMADDbcy;
    case 2: return Mips::VMADDbcz;
    case 3: return Mips::VMADDbcw;
    default: llvm_unreachable("Invalid broadcast component");
    }
  }

  /// Check if an instruction is loading a zero vector from constant pool.
  /// This handles the pattern: v4f32 result = {0,0,0,0}; which loads from CP
  /// since VF0 is {0,0,0,1} and cannot be used for all-zeros.
  bool isZeroVectorLoad(const MachineInstr &MI) const {
    // LQC2 from constant pool is the common pattern for zero vector init
    if (MI.getOpcode() != Mips::LQC2)
      return false;
    // Check if loading from constant pool (the MachineMemOperand will indicate)
    for (const MachineMemOperand *MMO : MI.memoperands()) {
      if (MMO->getPseudoValue() &&
          MMO->getPseudoValue()->kind() == PseudoSourceValue::ConstantPool)
        return true;
    }
    return false;
  }

  /// Represents a broadcast multiply leaf in the reduction tree
  struct BroadcastMulLeaf {
    MachineInstr *VMul;      // The VMULbc instruction
    Register LHS;            // First operand (vector)
    Register RHS;            // Second operand (broadcast source)
    unsigned BroadcastComp;  // 0=x, 1=y, 2=z, 3=w
  };

  /// Try to build a chain starting from a VADD root
  bool buildChain(MachineInstr &Root, SmallVectorImpl<BroadcastMulLeaf> &Leaves,
                  SmallVectorImpl<MachineInstr *> &VAdds,
                  DenseSet<MachineInstr *> &ChainInstrs);

  /// Transform a chain to use ACC instructions
  void transformChain(MachineBasicBlock &MBB, MachineInstr &Root,
                      SmallVectorImpl<BroadcastMulLeaf> &Leaves,
                      SmallVectorImpl<MachineInstr *> &VAdds);
};

} // namespace

INITIALIZE_PASS(MipsR5900VU0AccChain, DEBUG_TYPE,
                "R5900 VU0 Accumulator Chain Optimization", false, false)

char MipsR5900VU0AccChain::ID = 0;

bool MipsR5900VU0AccChain::runOnMachineFunction(MachineFunction &MF) {
  STI = &MF.getSubtarget<MipsSubtarget>();

  // Only run on R5900 with VU0
  if (!STI->isR5900() || !STI->hasVU0())
    return false;

  TII = STI->getInstrInfo();
  MRI = &MF.getRegInfo();

  LLVM_DEBUG(dbgs() << "Running R5900 VU0 ACC Chain pass on "
                    << MF.getName() << "\n");

  bool Modified = false;
  for (auto &MBB : MF)
    Modified |= optimizeBasicBlock(MBB);

  return Modified;
}

MachineInstr *MipsR5900VU0AccChain::findDef(Register Reg) {
  if (Reg.isVirtual())
    return MRI->getVRegDef(Reg);
  return nullptr;
}

bool MipsR5900VU0AccChain::buildChain(MachineInstr &Root,
                                       SmallVectorImpl<BroadcastMulLeaf> &Leaves,
                                       SmallVectorImpl<MachineInstr *> &VAdds,
                                       DenseSet<MachineInstr *> &ChainInstrs) {
  // Work list for BFS traversal of the reduction tree
  SmallVector<MachineInstr *, 8> Worklist;
  Worklist.push_back(&Root);

  // Limit chain depth
  constexpr unsigned MaxChainDepth = 16;
  unsigned Depth = 0;

  while (!Worklist.empty() && Depth < MaxChainDepth) {
    MachineInstr *MI = Worklist.pop_back_val();

    // Skip if already visited
    if (!ChainInstrs.insert(MI).second)
      continue;

    Depth++;

    if (isVMULbc(*MI)) {
      // Leaf node - this is a broadcast multiply
      BroadcastMulLeaf Leaf;
      Leaf.VMul = MI;
      Leaf.LHS = MI->getOperand(1).getReg();
      Leaf.RHS = MI->getOperand(2).getReg();
      Leaf.BroadcastComp = getBroadcastComponent(*MI);
      Leaves.push_back(Leaf);
    } else if (isVADD(*MI)) {
      VAdds.push_back(MI);

      // Get both operands
      Register Op1 = MI->getOperand(1).getReg();
      Register Op2 = MI->getOperand(2).getReg();

      // Find the defining instructions
      MachineInstr *Def1 = findDef(Op1);
      MachineInstr *Def2 = findDef(Op2);

      if (!Def1 || !Def2)
        return false;

      // Check if either operand is a zero vector load from constant pool.
      // This happens at the start of a sum: result = {0,0,0,0} + (m0 * v.x).
      // In this case, we only need to follow the non-zero operand.
      bool Def1IsZero = isZeroVectorLoad(*Def1);
      bool Def2IsZero = isZeroVectorLoad(*Def2);

      // If both are zero loads, not useful
      if (Def1IsZero && Def2IsZero)
        return false;

      // If one is zero, only follow the other operand
      if (Def1IsZero) {
        if (!isVMULbc(*Def2) && !isVADD(*Def2))
          return false;
        Worklist.push_back(Def2);
      } else if (Def2IsZero) {
        if (!isVMULbc(*Def1) && !isVADD(*Def1))
          return false;
        Worklist.push_back(Def1);
      } else {
        // Both operands must be VMULbc or VADD
        if (!isVMULbc(*Def1) && !isVADD(*Def1))
          return false;
        if (!isVMULbc(*Def2) && !isVADD(*Def2))
          return false;

        Worklist.push_back(Def1);
        Worklist.push_back(Def2);
      }
    } else {
      return false;
    }
  }

  // Need at least 2 broadcast multiplies to benefit from ACC
  if (Leaves.size() < 2)
    return false;

  // Verify that all intermediate results are only used within the chain.
  // If any intermediate value is used outside the chain, we cannot safely
  // delete the original instructions.
  for (MachineInstr *MI : ChainInstrs) {
    // Skip the root - its result is what we're replacing
    if (MI == &Root)
      continue;

    // Check all defs of this instruction
    for (const MachineOperand &MO : MI->defs()) {
      if (!MO.isReg() || !MO.getReg().isVirtual())
        continue;

      Register DefReg = MO.getReg();
      for (MachineInstr &Use : MRI->use_instructions(DefReg)) {
        // All uses must be within the chain
        if (!ChainInstrs.count(&Use)) {
          LLVM_DEBUG(dbgs() << "R5900 VU0 ACC: Cannot optimize - intermediate "
                            << "value has uses outside chain\n");
          return false;
        }
      }
    }
  }

  return true;
}

void MipsR5900VU0AccChain::transformChain(MachineBasicBlock &MBB,
                                           MachineInstr &Root,
                                           SmallVectorImpl<BroadcastMulLeaf> &Leaves,
                                           SmallVectorImpl<MachineInstr *> &VAdds) {
  LLVM_DEBUG(dbgs() << "R5900 VU0 ACC: Transforming chain with " << Leaves.size()
                    << " broadcast multiplies\n");

  // Reverse the leaves so we consume them in program order
  std::reverse(Leaves.begin(), Leaves.end());

  DebugLoc DL = Root.getDebugLoc();
  MachineBasicBlock::iterator InsertPt = Root;
  Register ResultReg = Root.getOperand(0).getReg();

  // Destination mask for full xyzw operation (0b1111 = 15)
  unsigned DestMask = 15;

  // First multiply -> VMULAbc (ACC = fs * ft[bc])
  // Operands: destmask, $ACC, $fs, $ft (ACC is explicit, VU0ACC is implicit def)
  BuildMI(MBB, InsertPt, DL, TII->get(getVMULAbcOpcode(Leaves[0].BroadcastComp)))
      .addImm(DestMask)           // dest mask (xyzw)
      .addReg(Mips::VU0ACC)       // ACC operand (explicit)
      .addReg(Leaves[0].LHS)      // fs
      .addReg(Leaves[0].RHS);     // ft

  // Middle multiplies -> VMADDAbc (ACC += fs * ft[bc])
  // Operands: destmask, $ACC, $fs, $ft (ACC is explicit, VU0ACC is implicit use/def)
  for (size_t i = 1; i < Leaves.size() - 1; ++i) {
    BuildMI(MBB, InsertPt, DL, TII->get(getVMADDAbcOpcode(Leaves[i].BroadcastComp)))
        .addImm(DestMask)           // dest mask (xyzw)
        .addReg(Mips::VU0ACC)       // ACC operand (explicit)
        .addReg(Leaves[i].LHS)      // fs
        .addReg(Leaves[i].RHS);     // ft
  }

  // Last multiply -> VMADDbc (fd = ACC + fs * ft[bc])
  // Operands: $fd, destmask, $fs, $ft (VU0ACC is implicit use)
  BuildMI(MBB, InsertPt, DL, TII->get(getVMADDbcOpcode(Leaves.back().BroadcastComp)), ResultReg)
      .addImm(DestMask)           // dest mask (xyzw)
      .addReg(Leaves.back().LHS)  // fs
      .addReg(Leaves.back().RHS); // ft

  // Remove the original instructions
  for (BroadcastMulLeaf &Leaf : Leaves)
    Leaf.VMul->eraseFromParent();
  for (MachineInstr *VAdd : VAdds)
    VAdd->eraseFromParent();

  // Update statistics
  NumVU0ChainsOptimized++;
  NumVU0InstructionsSaved += VAdds.size();
}

bool MipsR5900VU0AccChain::optimizeBasicBlock(MachineBasicBlock &MBB) {
  bool Modified = false;

  // Collect VADD instructions in reverse order
  SmallVector<MachineInstr *, 8> Candidates;
  for (MachineInstr &MI : reverse(MBB)) {
    if (isVADD(MI))
      Candidates.push_back(&MI);
  }

  // Track which instructions are already part of a chain
  DenseSet<MachineInstr *> ProcessedInstrs;

  // Try to build and transform chains
  for (MachineInstr *MI : Candidates) {
    // Skip if already processed as part of another chain
    if (ProcessedInstrs.count(MI))
      continue;

    // Skip if already erased
    if (MI->getParent() != &MBB)
      continue;

    SmallVector<BroadcastMulLeaf, 8> Leaves;
    SmallVector<MachineInstr *, 8> VAdds;
    DenseSet<MachineInstr *> ChainInstrs;

    if (buildChain(*MI, Leaves, VAdds, ChainInstrs)) {
      // Mark all chain instructions as processed
      ProcessedInstrs.insert(ChainInstrs.begin(), ChainInstrs.end());

      transformChain(MBB, *MI, Leaves, VAdds);
      Modified = true;
    }
  }

  return Modified;
}

FunctionPass *llvm::createMipsR5900VU0AccChainPass() {
  return new MipsR5900VU0AccChain();
}
