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
// Pattern: result = (a*b) + (c*d) - (e*f) + ...
// Becomes: MULA_S a, b
//          MADDA_S c, d
//          MSUBA_S e, f
//          MADD_S result, g, h
//
// Each step rounds as the add or subtract and the multiply that it replaces
// (measured on the PS2, bench/accprobe), so the terms are accumulated in the
// order of the evaluation: a sum such as (t0 + t1) - t2, where every add has
// one term as an operand. Only with reassociation on all of the adds may a
// sum have another shape, such as (t0 + t1) + (t2 + t3), and the terms be
// reordered: the products are then ordered by where their operands are
// defined, so the one that waits longest for an operand (often the result of
// the previous step) is the final MADD_S, and the chain does not wait for it.
//
// One term that is not a product, as in c0 + c1*x + c2*x^2 + y*x^3, is
// multiplied by 1.0 in ft (the R5900 multiplier is exact for a power of two
// in ft, but can be 1 ULP low with it in fs).
//
//===----------------------------------------------------------------------===//

#include "Mips.h"
#include "MipsInstrInfo.h"
#include "MipsSubtarget.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
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

/// A term of a sum: the product A * B (FMul), or a plain value A.
struct Term {
  Register A, B;
  MachineInstr *FMul = nullptr;
  bool Neg = false;
};

/// The terms of a sum, in the order of the evaluation, and its adds and
/// subtracts.
struct Chain {
  SmallVector<Term, 8> Terms;
  SmallVector<MachineInstr *, 8> Nodes;
  /// Every node has a term as an operand: the order of the terms is the
  /// order of the evaluation.
  bool Linear = true;
  /// Every node may be reassociated.
  bool Reassoc = true;
  /// The +0.0 terms that the sum leaves out.
  SmallVector<MachineInstr *, 2> Zeros;
};

class MipsR5900FPUAccChain : public MachineFunctionPass {
public:
  MipsR5900FPUAccChain() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return "R5900 FPU Accumulator Chain Optimization";
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  static char ID;

private:
  const MipsInstrInfo *TII = nullptr;
  const MipsSubtarget *STI = nullptr;
  MachineRegisterInfo *MRI = nullptr;
  /// 1.0f in an FPR, made once per function when a chain needs it.
  Register One;

  bool isNode(Register Reg, const MachineBasicBlock &MBB) const;
  bool collect(Register Reg, bool Neg, const MachineBasicBlock &MBB,
               Chain &C) const;
  bool buildChain(MachineInstr &Root, Chain &C) const;
  void transformChain(MachineBasicBlock &MBB, MachineInstr &Root, Chain &C);
  bool optimizeBasicBlock(MachineBasicBlock &MBB);
  /// The register with 1.0f, made in the entry block on first use.
  Register getOne(MachineFunction &MF);
};

} // namespace

INITIALIZE_PASS(MipsR5900FPUAccChain, DEBUG_TYPE,
                "R5900 FPU Accumulator Chain Optimization", false, false)

char MipsR5900FPUAccChain::ID = 0;

bool MipsR5900FPUAccChain::runOnMachineFunction(MachineFunction &MF) {
  STI = &MF.getSubtarget<MipsSubtarget>();
  if (!STI->isR5900())
    return false;
  TII = STI->getInstrInfo();
  MRI = &MF.getRegInfo();
  One = Register();

  bool Modified = false;
  for (auto &MBB : MF)
    Modified |= optimizeBasicBlock(MBB);
  return Modified;
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

/// An add or subtract in \p MBB whose only reader is the sum.
bool MipsR5900FPUAccChain::isNode(Register Reg,
                                  const MachineBasicBlock &MBB) const {
  if (!Reg.isVirtual() || !MRI->hasOneNonDBGUse(Reg))
    return false;
  const MachineInstr *Def = MRI->getVRegDef(Reg);
  return Def && Def->getParent() == &MBB &&
         (Def->getOpcode() == Mips::FADD_S || Def->getOpcode() == Mips::FSUB_S);
}

static bool isZeroConstant(const MachineInstr *MI) {
  return MI && MI->getOpcode() == Mips::MTC1 && MI->getOperand(1).isReg() &&
         MI->getOperand(1).getReg() == Mips::R0;
}

/// Collects the terms of the value \p Reg, which the sum adds (\p Neg false)
/// or subtracts, depth first with the operand that is a sum first.
bool MipsR5900FPUAccChain::collect(Register Reg, bool Neg,
                                   const MachineBasicBlock &MBB,
                                   Chain &C) const {
  MachineInstr *Def = MRI->getVRegDef(Reg);
  if (isNode(Reg, MBB)) {
    C.Nodes.push_back(Def);
    C.Reassoc &= Def->getFlag(MachineInstr::FmReassoc);
    Register L = Def->getOperand(1).getReg(), R = Def->getOperand(2).getReg();
    bool Sub = Def->getOpcode() == Mips::FSUB_S;
    // +0.0 + t is t, up to the sign of a zero.
    if (!Sub && Def->getFlag(MachineInstr::FmNsz)) {
      for (auto [Zero, Other] : {std::pair(L, R), std::pair(R, L)})
        if (isZeroConstant(MRI->getVRegDef(Zero))) {
          C.Zeros.push_back(MRI->getVRegDef(Zero));
          return collect(Other, Neg, MBB, C);
        }
    }
    bool LNode = isNode(L, MBB), RNode = isNode(R, MBB);
    if (LNode && RNode)
      C.Linear = false;
    if (!LNode && RNode) {
      // t - (sum) negates the sum; an add is commutative.
      if (Sub)
        C.Linear = false;
      else
        std::swap(L, R);
    }
    return collect(L, Neg, MBB, C) && collect(R, Neg != Sub, MBB, C);
  }
  Term T;
  T.Neg = Neg;
  if (Def && Def->getParent() == &MBB && Def->getOpcode() == Mips::FMUL_S &&
      MRI->hasOneNonDBGUse(Reg)) {
    T.A = Def->getOperand(1).getReg();
    T.B = Def->getOperand(2).getReg();
    T.FMul = Def;
  } else {
    if (MRI->getRegClass(Reg) != &Mips::FGR32RegClass)
      return false;
    T.A = Reg;
  }
  C.Terms.push_back(T);
  return true;
}

bool MipsR5900FPUAccChain::buildChain(MachineInstr &Root, Chain &C) const {
  const MachineBasicBlock &MBB = *Root.getParent();
  // The root is a node whatever reads it.
  C.Nodes.push_back(&Root);
  C.Reassoc = Root.getFlag(MachineInstr::FmReassoc);
  Register L = Root.getOperand(1).getReg(), R = Root.getOperand(2).getReg();
  bool Sub = Root.getOpcode() == Mips::FSUB_S;
  bool LNode = isNode(L, MBB), RNode = isNode(R, MBB);
  if (LNode && RNode)
    C.Linear = false;
  if (!LNode && RNode) {
    if (Sub)
      C.Linear = false;
    else
      std::swap(L, R);
  }
  if (!collect(L, false, MBB, C) || !collect(R, Sub, MBB, C))
    return false;
  if (!C.Linear && !C.Reassoc)
    return false;

  // At least 2 products: with a plain term, the chain saves one instruction
  // per multiply but one.
  unsigned Products = count_if(C.Terms, [](const Term &T) { return T.FMul; });
  if (Products < 2 || C.Terms.size() - Products > 1)
    return false;
  // ACC can not start negated.
  if (!C.Reassoc && C.Terms[0].Neg)
    return false;
  if (none_of(C.Terms, [](const Term &T) { return !T.Neg; }))
    return false;
  return true;
}

void MipsR5900FPUAccChain::transformChain(MachineBasicBlock &MBB,
                                          MachineInstr &Root, Chain &C) {
  LLVM_DEBUG(dbgs() << "R5900 FPU ACC: Transforming chain with "
                    << C.Terms.size() << " terms\n");
  if (C.Reassoc) {
    // The plain term first, then the products by the position of the latest
    // definition of their operands in this block (earlier blocks first), so
    // that the chain starts as soon as possible and the multiply that waits
    // longest comes last. The first term is not subtracted.
    DenseMap<const MachineInstr *, unsigned> Pos;
    unsigned N = 0;
    for (MachineInstr &MI : MBB)
      Pos[&MI] = ++N;
    auto ReadyAt = [&](const Term &T) {
      if (!T.FMul)
        return 0u;
      unsigned At = 0;
      for (Register R : {T.A, T.B})
        if (MachineInstr *Def = MRI->getVRegDef(R))
          if (Def->getParent() == &MBB)
            At = std::max(At, Pos.lookup(Def));
      return At;
    };
    llvm::stable_sort(C.Terms, [&](const Term &A, const Term &B) {
      return ReadyAt(A) < ReadyAt(B);
    });
    auto *First = find_if(C.Terms, [](const Term &T) { return !T.Neg; });
    std::rotate(C.Terms.begin(), First, std::next(First));
  }

  DebugLoc DL = Root.getDebugLoc();
  MachineBasicBlock::iterator InsertPt = Root;
  auto Operands = [&](MachineInstrBuilder MIB, const Term &T) {
    MIB.addReg(T.A).addReg(T.FMul ? T.B : getOne(*MBB.getParent()));
  };
  Operands(BuildMI(MBB, InsertPt, DL, TII->get(Mips::MULA_S)), C.Terms[0]);
  for (const Term &T : drop_end(drop_begin(C.Terms)))
    Operands(BuildMI(MBB, InsertPt, DL,
                     TII->get(T.Neg ? Mips::MSUBA_S : Mips::MADDA_S)),
             T);
  const Term &Last = C.Terms.back();
  Operands(BuildMI(MBB, InsertPt, DL,
                   TII->get(Last.Neg ? Mips::R5900_MSUB_S : Mips::R5900_MADD_S),
                   Root.getOperand(0).getReg()),
           Last);

  for (const Term &T : C.Terms)
    if (T.FMul)
      T.FMul->eraseFromParent();
  for (MachineInstr *Node : C.Nodes)
    Node->eraseFromParent();
  for (MachineInstr *Zero : C.Zeros)
    if (MRI->use_nodbg_empty(Zero->getOperand(0).getReg()))
      Zero->eraseFromParent();
  NumChainsOptimized++;
  NumInstructionsSaved += C.Nodes.size();
}

bool MipsR5900FPUAccChain::optimizeBasicBlock(MachineBasicBlock &MBB) {
  // A loop that keeps a sum in ACC (MipsR5900LoopAcc): ACC is live through
  // the block.
  if (MBB.isLiveIn(Mips::FPU_ACC) &&
      any_of(MBB.successors(), [](const MachineBasicBlock *S) {
        return S->isLiveIn(Mips::FPU_ACC);
      }))
    return false;

  // The roots of the sums: the adds and subtracts, last first, so that the
  // largest sum is taken before its parts.
  SmallVector<MachineInstr *, 8> Candidates;
  for (MachineInstr &MI : reverse(MBB))
    if (MI.getOpcode() == Mips::FADD_S || MI.getOpcode() == Mips::FSUB_S)
      Candidates.push_back(&MI);

  bool Modified = false;
  SmallPtrSet<MachineInstr *, 16> Erased;
  for (MachineInstr *MI : Candidates) {
    if (Erased.count(MI))
      continue;
    Chain C;
    if (!buildChain(*MI, C))
      continue;
    Erased.insert(C.Nodes.begin(), C.Nodes.end());
    transformChain(MBB, *MI, C);
    Modified = true;
  }
  return Modified;
}

FunctionPass *llvm::createMipsR5900FPUAccChainPass() {
  return new MipsR5900FPUAccChain();
}
