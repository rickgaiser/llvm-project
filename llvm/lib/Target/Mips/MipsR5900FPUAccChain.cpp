//===- MipsR5900FPUAccChain.cpp - R5900 FPU ACC Chain Optimization --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass recognizes chains of floating-point multiply-accumulate operations
// and converts them to use the R5900 FPU accumulator instructions.
//
// Pattern: result = (a*b) + (c*d) + (e*f) + ...
// Becomes: MULA_S a, b
//          MADDA_S c, d
//          MADDA_S e, f
//          MADD_S result, g, h
//
// The R5900 (PS2 Emotion Engine) has a dedicated FPU accumulator register that
// enables efficient multiply-accumulate chains for operations like dot products
// and matrix multiplications.
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

#define DEBUG_TYPE "mips-r5900-fpu-acc-chain"

using namespace llvm;

STATISTIC(NumChainsOptimized, "Number of FPU ACC chains optimized");
STATISTIC(NumInstructionsSaved, "Number of instructions saved");

namespace {

class MipsR5900FPUAccChain : public MachineFunctionPass {
public:
  MipsR5900FPUAccChain() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return "R5900 FPU Accumulator Chain Optimization";
  }

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoVRegs();
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  static char ID;

private:
  const MipsInstrInfo *TII = nullptr;
  const MipsSubtarget *STI = nullptr;
  MachineRegisterInfo *MRI = nullptr;

  /// Find the instruction that defines Reg before the given instruction
  MachineInstr *findDefBefore(MachineInstr &Use, Register Reg);

  /// Optimize multiply-add chains in a basic block
  bool optimizeBasicBlock(MachineBasicBlock &MBB);

  /// Check if an instruction is FADD_S
  bool isFAddS(const MachineInstr &MI) const {
    return MI.getOpcode() == Mips::FADD_S;
  }

  /// Check if an instruction is FMUL_S
  bool isFMulS(const MachineInstr &MI) const {
    return MI.getOpcode() == Mips::FMUL_S;
  }

  /// Represents a multiply-add leaf in the reduction tree
  struct MulAddLeaf {
    MachineInstr *FMul;  // The FMUL_S instruction
    Register LHS;        // First operand of multiply
    Register RHS;        // Second operand of multiply
  };

  /// Try to build a chain starting from a FADD_S root
  bool buildChain(MachineInstr &Root, SmallVectorImpl<MulAddLeaf> &Leaves,
                  SmallVectorImpl<MachineInstr *> &FAdds,
                  DenseSet<MachineInstr *> &ChainInstrs);

  /// Transform a chain to use ACC instructions
  void transformChain(MachineBasicBlock &MBB, MachineInstr &Root,
                      SmallVectorImpl<MulAddLeaf> &Leaves,
                      SmallVectorImpl<MachineInstr *> &FAdds);
};

} // namespace

INITIALIZE_PASS(MipsR5900FPUAccChain, DEBUG_TYPE,
                "R5900 FPU Accumulator Chain Optimization", false, false)

char MipsR5900FPUAccChain::ID = 0;

bool MipsR5900FPUAccChain::runOnMachineFunction(MachineFunction &MF) {
  STI = &MF.getSubtarget<MipsSubtarget>();

  // Only run on R5900
  if (!STI->isR5900())
    return false;

  TII = STI->getInstrInfo();
  MRI = &MF.getRegInfo();

  LLVM_DEBUG(dbgs() << "Running R5900 FPU ACC Chain pass on "
                    << MF.getName() << "\n");

  bool Modified = false;
  for (auto &MBB : MF)
    Modified |= optimizeBasicBlock(MBB);

  return Modified;
}

MachineInstr *MipsR5900FPUAccChain::findDefBefore(MachineInstr &Use,
                                                   Register Reg) {
  // Scan backwards from Use to find the instruction that defines Reg
  MachineBasicBlock *MBB = Use.getParent();
  MachineBasicBlock::reverse_iterator RI(Use.getIterator());
  for (MachineBasicBlock::reverse_iterator RE = MBB->rend(); RI != RE; ++RI) {
    MachineInstr &MI = *RI;
    for (const MachineOperand &MO : MI.defs()) {
      if (MO.isReg() && MO.getReg() == Reg) {
        return &MI;
      }
    }
  }
  return nullptr;
}

bool MipsR5900FPUAccChain::buildChain(MachineInstr &Root,
                                       SmallVectorImpl<MulAddLeaf> &Leaves,
                                       SmallVectorImpl<MachineInstr *> &FAdds,
                                       DenseSet<MachineInstr *> &ChainInstrs) {
  // Work list for BFS traversal of the reduction tree
  SmallVector<MachineInstr *, 8> Worklist;
  Worklist.push_back(&Root);

  // Limit chain depth to prevent runaway behavior
  constexpr unsigned MaxChainDepth = 16;
  unsigned Depth = 0;

  while (!Worklist.empty() && Depth < MaxChainDepth) {
    MachineInstr *MI = Worklist.pop_back_val();

    // Skip if already visited
    if (!ChainInstrs.insert(MI).second)
      continue;

    Depth++;

    if (isFMulS(*MI)) {
      // Leaf node - this is a multiply
      MulAddLeaf Leaf;
      Leaf.FMul = MI;
      Leaf.LHS = MI->getOperand(1).getReg();
      Leaf.RHS = MI->getOperand(2).getReg();
      Leaves.push_back(Leaf);
    } else if (isFAddS(*MI)) {
      FAdds.push_back(MI);

      // Get both operands
      Register Op1 = MI->getOperand(1).getReg();
      Register Op2 = MI->getOperand(2).getReg();

      // Find the defining instructions by scanning backwards from this use
      MachineInstr *Def1 = findDefBefore(*MI, Op1);
      MachineInstr *Def2 = findDefBefore(*MI, Op2);

      if (!Def1 || !Def2)
        return false;

      // Both operands must be FMUL_S or FADD_S
      if (!isFMulS(*Def1) && !isFAddS(*Def1))
        return false;
      if (!isFMulS(*Def2) && !isFAddS(*Def2))
        return false;

      Worklist.push_back(Def1);
      Worklist.push_back(Def2);
    } else {
      return false;
    }
  }

  // Need at least 2 multiplies to benefit from ACC
  return Leaves.size() >= 2;
}

void MipsR5900FPUAccChain::transformChain(MachineBasicBlock &MBB,
                                           MachineInstr &Root,
                                           SmallVectorImpl<MulAddLeaf> &Leaves,
                                           SmallVectorImpl<MachineInstr *> &FAdds) {
  LLVM_DEBUG(dbgs() << "R5900 FPU ACC: Transforming chain with " << Leaves.size()
                    << " multiplies\n");

  DebugLoc DL = Root.getDebugLoc();
  MachineBasicBlock::iterator InsertPt = Root;
  Register ResultReg = Root.getOperand(0).getReg();

  // First multiply -> MULA_S (ACC = a * b)
  BuildMI(MBB, InsertPt, DL, TII->get(Mips::MULA_S))
      .addReg(Leaves[0].LHS)
      .addReg(Leaves[0].RHS);

  // Middle multiplies -> MADDA_S (ACC += c * d)
  for (size_t i = 1; i < Leaves.size() - 1; ++i) {
    BuildMI(MBB, InsertPt, DL, TII->get(Mips::MADDA_S))
        .addReg(Leaves[i].LHS)
        .addReg(Leaves[i].RHS);
  }

  // Last multiply -> MADD_S (result = ACC + e * f)
  BuildMI(MBB, InsertPt, DL, TII->get(Mips::R5900_MADD_S), ResultReg)
      .addReg(Leaves.back().LHS)
      .addReg(Leaves.back().RHS);

  // Remove the original instructions
  for (MulAddLeaf &Leaf : Leaves)
    Leaf.FMul->eraseFromParent();
  for (MachineInstr *FAdd : FAdds)
    FAdd->eraseFromParent();

  // Update statistics
  NumChainsOptimized++;
  // We saved: (N-1) FADD_S instructions
  NumInstructionsSaved += FAdds.size();
}

bool MipsR5900FPUAccChain::optimizeBasicBlock(MachineBasicBlock &MBB) {
  bool Modified = false;

  // Collect FADD_S instructions in REVERSE order. This ensures we process
  // the outermost chains first (the last FADD_S in program order is likely
  // the root of the longest chain).
  SmallVector<MachineInstr *, 8> Candidates;
  for (MachineInstr &MI : reverse(MBB)) {
    if (isFAddS(MI))
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

    SmallVector<MulAddLeaf, 8> Leaves;
    SmallVector<MachineInstr *, 8> FAdds;
    DenseSet<MachineInstr *> ChainInstrs;

    if (buildChain(*MI, Leaves, FAdds, ChainInstrs)) {
      // Mark all chain instructions as processed
      ProcessedInstrs.insert(ChainInstrs.begin(), ChainInstrs.end());

      transformChain(MBB, *MI, Leaves, FAdds);
      Modified = true;
    }
  }

  return Modified;
}

FunctionPass *llvm::createMipsR5900FPUAccChainPass() {
  return new MipsR5900FPUAccChain();
}
