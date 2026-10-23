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
// One term that is not a product, as in c0 + c1*x + c2*x^2 + y*x^3, starts
// the chain as MULA_S c0, 1.0 (1.0 second: the R5900 multiplier is exact for a
// power of two in ft, but can be 1 ULP low with it in fs). The products are
// ordered by where their operands are defined, so the one that waits longest
// for an operand (y above, often the result of the previous step) is the
// final MADD_S, and the chain does not wait for it.
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

  // This pass runs before register allocation, so virtual registers are used.
  // No special properties required.

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

  /// Check if an instruction is MTC1 with $zero (i.e., loading 0.0f)
  bool isZeroConstant(const MachineInstr &MI) const {
    return MI.getOpcode() == Mips::MTC1 && MI.getOperand(1).isReg() &&
           MI.getOperand(1).getReg() == Mips::R0;
  }

  /// Represents a multiply-add leaf in the reduction tree
  struct MulAddLeaf {
    MachineInstr *FMul; // The FMUL_S instruction
    Register LHS;       // First operand of multiply
    Register RHS;       // Second operand of multiply
  };

  /// 1.0f in an FPR, made once per function when a chain needs it.
  Register One;

  /// Try to build a chain starting from a FADD_S root. Plain is set to the
  /// one term that is not a product, if any.
  bool buildChain(MachineInstr &Root, SmallVectorImpl<MulAddLeaf> &Leaves,
                  SmallVectorImpl<MachineInstr *> &FAdds,
                  DenseSet<MachineInstr *> &ChainInstrs, Register &Plain);

  /// Transform a chain to use ACC instructions
  void transformChain(MachineBasicBlock &MBB, MachineInstr &Root,
                      SmallVectorImpl<MulAddLeaf> &Leaves,
                      SmallVectorImpl<MachineInstr *> &FAdds, Register Plain);

  /// The register with 1.0f, made in the entry block on first use.
  Register getOne(MachineFunction &MF);
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
  One = Register();

  LLVM_DEBUG(dbgs() << "Running R5900 FPU ACC Chain pass on " << MF.getName()
                    << "\n");

  bool Modified = false;
  for (auto &MBB : MF)
    Modified |= optimizeBasicBlock(MBB);

  return Modified;
}

MachineInstr *MipsR5900FPUAccChain::findDefBefore(MachineInstr &Use,
                                                  Register Reg) {
  // For virtual registers (before register allocation), use getVRegDef()
  // which correctly returns the unique defining instruction in SSA form.
  if (Reg.isVirtual())
    return MRI->getVRegDef(Reg);

  // For physical registers (shouldn't happen when running before regalloc),
  // scan backwards from Use to find the instruction that defines Reg
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

Register MipsR5900FPUAccChain::getOne(MachineFunction &MF) {
  if (One)
    return One;
  MachineBasicBlock &Entry = MF.front();
  MachineBasicBlock::iterator InsertPt = Entry.getFirstNonPHI();
  Register Bits = MRI->createVirtualRegister(&Mips::GPR32RegClass);
  One = MRI->createVirtualRegister(&Mips::FGR32RegClass);
  BuildMI(Entry, InsertPt, DebugLoc(), TII->get(Mips::LUi), Bits)
      .addImm(0x3f80);
  BuildMI(Entry, InsertPt, DebugLoc(), TII->get(Mips::MTC1), One).addReg(Bits);
  return One;
}

bool MipsR5900FPUAccChain::buildChain(MachineInstr &Root,
                                      SmallVectorImpl<MulAddLeaf> &Leaves,
                                      SmallVectorImpl<MachineInstr *> &FAdds,
                                      DenseSet<MachineInstr *> &ChainInstrs,
                                      Register &Plain) {
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

      // Check if either operand is a zero constant (MTC1 $zero).
      // This happens at the start of a sum: sum = 0.0f + (a*b).
      // In this case, we only need to follow the non-zero operand.
      bool Def1IsZero = isZeroConstant(*Def1);
      bool Def2IsZero = isZeroConstant(*Def2);

      // If both are zero, this is just 0+0, not useful
      if (Def1IsZero && Def2IsZero)
        return false;

      // If one is zero, only follow the other operand
      if (Def1IsZero) {
        if (!isFMulS(*Def2) && !isFAddS(*Def2))
          return false;
        Worklist.push_back(Def2);
      } else if (Def2IsZero) {
        if (!isFMulS(*Def1) && !isFAddS(*Def1))
          return false;
        Worklist.push_back(Def1);
      } else {
        // Both operands are FMUL_S or FADD_S, except for at most one term of
        // the whole chain, which starts it as MULA_S term, 1.0.
        for (auto [Def, Op] : {std::pair(Def1, Op1), std::pair(Def2, Op2)}) {
          if (isFMulS(*Def) || isFAddS(*Def)) {
            Worklist.push_back(Def);
            continue;
          }
          if (Plain || MRI->getRegClass(Op) != &Mips::FGR32RegClass)
            return false;
          Plain = Op;
        }
      }
    } else {
      return false;
    }
  }

  // Need at least 2 multiplies to benefit from ACC (with a plain term, the
  // chain saves one instruction per multiply but one)
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
          LLVM_DEBUG(dbgs() << "R5900 FPU ACC: Cannot optimize - intermediate "
                            << "value has uses outside chain\n");
          return false;
        }
      }
    }
  }

  return true;
}

void MipsR5900FPUAccChain::transformChain(
    MachineBasicBlock &MBB, MachineInstr &Root,
    SmallVectorImpl<MulAddLeaf> &Leaves, SmallVectorImpl<MachineInstr *> &FAdds,
    Register Plain) {
  LLVM_DEBUG(dbgs() << "R5900 FPU ACC: Transforming chain with "
                    << Leaves.size() << " multiplies"
                    << (Plain ? " and a plain term" : "") << "\n");

  // Order the multiplies by the position of the latest definition of their
  // operands in this block (earlier blocks first), so that the chain starts
  // as soon as possible and the multiply that waits longest comes last.
  DenseMap<const MachineInstr *, unsigned> Pos;
  unsigned N = 0;
  for (MachineInstr &MI : MBB)
    Pos[&MI] = ++N;
  auto ReadyAt = [&](const MulAddLeaf &L) {
    unsigned At = 0;
    for (Register R : {L.LHS, L.RHS})
      if (MachineInstr *Def = MRI->getVRegDef(R))
        if (Def->getParent() == &MBB)
          At = std::max(At, Pos.lookup(Def));
    return At;
  };
  std::stable_sort(Leaves.begin(), Leaves.end(),
                   [&](const MulAddLeaf &A, const MulAddLeaf &B) {
                     return ReadyAt(A) < ReadyAt(B);
                   });

  DebugLoc DL = Root.getDebugLoc();
  MachineBasicBlock::iterator InsertPt = Root;
  Register ResultReg = Root.getOperand(0).getReg();

  // The start: ACC = plain * 1.0, or ACC = a * b
  size_t First = 0;
  if (Plain) {
    BuildMI(MBB, InsertPt, DL, TII->get(Mips::MULA_S))
        .addReg(Plain)
        .addReg(getOne(*MBB.getParent()));
  } else {
    BuildMI(MBB, InsertPt, DL, TII->get(Mips::MULA_S))
        .addReg(Leaves[0].LHS)
        .addReg(Leaves[0].RHS);
    First = 1;
  }

  // Middle multiplies -> MADDA_S (ACC += c * d)
  for (size_t i = First; i < Leaves.size() - 1; ++i) {
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
    Register Plain;

    if (buildChain(*MI, Leaves, FAdds, ChainInstrs, Plain)) {
      // Mark all chain instructions as processed
      ProcessedInstrs.insert(ChainInstrs.begin(), ChainInstrs.end());

      transformChain(MBB, *MI, Leaves, FAdds, Plain);
      Modified = true;
    }
  }

  return Modified;
}

FunctionPass *llvm::createMipsR5900FPUAccChainPass() {
  return new MipsR5900FPUAccChain();
}
