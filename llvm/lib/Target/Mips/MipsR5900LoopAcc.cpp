//===- MipsR5900LoopAcc.cpp - R5900 sums kept in an accumulator ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A sum of products that a single block loop carries from one iteration to
// the next, such as a dot product, is kept in an accumulator of the R5900
// for the whole loop, instead of being read from it and added in every
// iteration:
//
//   loop: s = PHI s0, s'                   ACC = s0
//         m = MUL.S a, b              ->   loop: MADDA.S a, b
//         s' = ADD.S s, m                    ...
//   exit: ... s' ...                       exit: d = MSUB.S 0, 0 (ACC - 0)
//
// The recurrence of the loop is then the ACC to ACC forwarding of the
// multiply-add (1 cycle for the FPU and VU0, 2 for MMI) instead of the
// latency of an add (4 cycles), and the add takes no issue slot.
//
// - FPU: FADD.S and FSUB.S of FMUL.S, in the physical FPU ACC: MADDA.S and
//   MSUBA.S. A term that is not a product is multiplied by 1.0 in ft, which
//   the R5900 multiplier gets exact.
// - VU0: VADD and VSUB of VMUL (or a broadcast VMULbc), in an ACC value
//   (VU0ACCRC): VMADDA and VMSUBA. The register allocator copies the start
//   value into ACC and the result out of it.
// - MMI: PMULTH and PMADDH, whose products the loop reads with PMFLO and
//   PMFHI and adds to two v4i32 sums: the products accumulate in HI and LO
//   with PMADDH, and HI and LO are read once after the loop. HI and LO are
//   one value of HILO128RC here, which the register allocator keeps apart
//   from the other uses of HI and LO.
//
// Measured on the PS2 (bench/accprobe): MADDA.S and VMADDA chains give the
// same bits as a multiply and an add per step, also with the start value
// moved in with MULA.S s0, 1.0, ADDA.S s0, 0.0 or VMULAw and the result moved
// out with MSUB.S d, 0, 0 or VMSUBx d, vf0, vf0.
//
// Without reassociation, the terms of an iteration must be added to the sum
// one after another, as in (s + a * b) + c * d. A loop vectorizer that
// interleaves the loop keeps several sums that are only added together
// after it; with reassociation they become one sum in ACC.
//
//===----------------------------------------------------------------------===//

#include "Mips.h"
#include "MipsInstrInfo.h"
#include "MipsR5900Sums.h"
#include "MipsSubtarget.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "mips-r5900-loop-acc"

STATISTIC(NumFPU, "Number of loop sums kept in the FPU ACC");
STATISTIC(NumVU0, "Number of loop sums kept in the VU0 ACC");
STATISTIC(NumMMI, "Number of loop sums kept in HI and LO");

static cl::opt<bool>
    DisableLoopAcc("disable-mips-r5900-loop-acc", cl::Hidden, cl::init(false),
                   cl::desc("Do not keep the sums of products of R5900 loops "
                            "in an accumulator"));

namespace {

/// A single block loop, its only predecessor outside the loop and its only
/// successor outside the loop.
struct SimpleLoop {
  MachineBasicBlock *Body, *Preheader, *Exit;
};

/// A value that the loop carries: Phi = PHI Init (Preheader), Next (Body).
struct LoopSum {
  MachineInstr *Phi;
  Register Init, Next;
};

/// A term t of a sum s + t or s - t: a product A * B, or a plain value A
/// (B is invalid). Field is the broadcast field of B for VU0 (0-3), or -1.
struct Term {
  Register A, B;
  int Field = -1;
  bool Neg = false;
};

/// How a unit adds and multiplies, for collectTerms.
struct SumOps {
  /// Returns true if MI adds (Neg false) or subtracts (Neg true) its
  /// operands \p L and \p R.
  function_ref<bool(const MachineInstr &MI, Register &L, Register &R,
                    bool &Neg)>
      IsAdd;
  /// Returns true if MI multiplies; sets T.A, T.B and T.Field.
  function_ref<bool(const MachineInstr &MI, Term &T)> IsMul;
  /// Integer adds: any order of the terms gives the same sum.
  bool Associative = false;
};

class MipsR5900LoopAcc : public MachineFunctionPass {
public:
  static char ID;
  MipsR5900LoopAcc() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "R5900 Loop Accumulators"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  const MipsSubtarget *STI = nullptr;
  const MipsInstrInfo *TII = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  MachineRegisterInfo *MRI = nullptr;

  std::optional<SimpleLoop> getLoop(MachineBasicBlock &MBB) const;
  SmallVector<LoopSum, 4> getSums(const SimpleLoop &L,
                                  const TargetRegisterClass *RC) const;
  bool usesReg(const SimpleLoop &L, MCRegister Reg) const;
  bool collectTerms(const SimpleLoop &L, const LoopSum &S, const SumOps &Ops,
                    SmallVectorImpl<Term> &Terms,
                    SmallVectorImpl<MachineInstr *> &Dead, bool &Reassoc) const;
  bool findExitSum(const SimpleLoop &L, ArrayRef<Register> Leaves,
                   function_ref<bool(const MachineInstr &)> IsCombine,
                   Register &Root, SmallVectorImpl<MachineInstr *> &Tree) const;
  bool prepareEdges(SimpleLoop &L);
  void replaceOutsideUses(const SimpleLoop &L, Register Old, Register New);
  bool isZero(Register Reg) const;

  bool convertFPU(SimpleLoop L);
  bool convertVU0(SimpleLoop L);
  bool convertVU0Chain(SimpleLoop L);
  bool convertMMI(SimpleLoop L);
};

} // end anonymous namespace

char MipsR5900LoopAcc::ID = 0;

INITIALIZE_PASS(MipsR5900LoopAcc, DEBUG_TYPE, "R5900 Loop Accumulators", false,
                false)

FunctionPass *llvm::createMipsR5900LoopAccPass() {
  return new MipsR5900LoopAcc();
}

std::optional<SimpleLoop>
MipsR5900LoopAcc::getLoop(MachineBasicBlock &MBB) const {
  if (!MBB.isSuccessor(&MBB) || MBB.succ_size() != 2 || MBB.pred_size() != 2)
    return std::nullopt;
  SimpleLoop L{&MBB, nullptr, nullptr};
  for (MachineBasicBlock *P : MBB.predecessors())
    if (P != &MBB)
      L.Preheader = P;
  for (MachineBasicBlock *S : MBB.successors())
    if (S != &MBB)
      L.Exit = S;
  if (!L.Preheader || !L.Exit || L.Exit->isEHPad())
    return std::nullopt;
  return L;
}

/// The values of class \p RC that the loop carries, whose start value is
/// read only by the computation of the next one, and whose next value is
/// read in the loop only by the PHI.
SmallVector<LoopSum, 4>
MipsR5900LoopAcc::getSums(const SimpleLoop &L,
                          const TargetRegisterClass *RC) const {
  SmallVector<LoopSum, 4> Sums;
  for (MachineInstr &Phi : L.Body->phis()) {
    Register Reg = Phi.getOperand(0).getReg();
    if (MRI->getRegClass(Reg) != RC || Phi.getNumOperands() != 5)
      continue;
    LoopSum S{&Phi, Register(), Register()};
    for (unsigned I = 1; I < 5; I += 2)
      (Phi.getOperand(I + 1).getMBB() == L.Body ? S.Next : S.Init) =
          Phi.getOperand(I).getReg();
    MachineInstr *NextDef = MRI->getVRegDef(S.Next);
    if (!NextDef || NextDef->getParent() != L.Body || NextDef->isPHI() ||
        !MRI->hasOneNonDBGUse(Reg))
      continue;
    if (any_of(MRI->use_nodbg_instructions(S.Next), [&](MachineInstr &MI) {
          return MI.getParent() == L.Body && &MI != &Phi;
        }))
      continue;
    Sums.push_back(S);
  }
  return Sums;
}

/// Returns true if an instruction of the loop reads or writes \p Reg.
bool MipsR5900LoopAcc::usesReg(const SimpleLoop &L, MCRegister Reg) const {
  for (const MachineInstr &MI : *L.Body)
    for (const MachineOperand &MO : MI.operands())
      if (MO.isReg() && MO.getReg().isPhysical() &&
          TRI->regsOverlap(MO.getReg(), Reg))
        return true;
  return false;
}

/// Collects the terms that the loop adds to the sum \p S in an iteration, in
/// the order of the evaluation, and the instructions that compute them.
/// \p Reassoc is set if the terms are not added one after another, which
/// needs reassociation to keep them in ACC.
bool MipsR5900LoopAcc::collectTerms(const SimpleLoop &L, const LoopSum &S,
                                    const SumOps &Ops,
                                    SmallVectorImpl<Term> &Terms,
                                    SmallVectorImpl<MachineInstr *> &Dead,
                                    bool &Reassoc) const {
  Register PhiReg = S.Phi->getOperand(0).getReg();
  MachineSumTerms Sum;
  collectMachineSumTerms(
      S.Next, *MRI,
      [&](Register Reg) -> std::optional<MachineSumNode> {
        // An add or subtract in the loop that only the sum reads.
        MachineInstr *Def = MRI->getVRegDef(Reg);
        MachineSumNode N;
        if (!Def || Def->getParent() != L.Body ||
            !Ops.IsAdd(*Def, N.L, N.R, N.Sub) ||
            (Reg != S.Next && !MRI->hasOneNonDBGUse(Reg)))
          return std::nullopt;
        return N;
      },
      [&](Register Reg) { return Reg == PhiReg; }, Sum);
  // ACC holds s, not -s, once.
  auto IsPhi = [&](const MachineSumTerm &T) { return T.Reg == PhiReg; };
  if (count_if(Sum.Terms, IsPhi) != 1 || find_if(Sum.Terms, IsPhi)->Neg)
    return false;
  // Accumulated from s on, the terms must follow s in the order of the
  // evaluation, or be reassociated.
  Reassoc = !Sum.Linear || Sum.Terms[0].Reg != PhiReg;
  if (Reassoc && !Sum.Reassoc && !Ops.Associative)
    return false;
  Dead.append(Sum.Nodes.begin(), Sum.Nodes.end());
  for (const MachineSumTerm &ST : Sum.Terms) {
    if (ST.Reg == PhiReg)
      continue;
    // A product that only the sum reads, or a plain value.
    Term T;
    T.Neg = ST.Neg;
    MachineInstr *Def = MRI->getVRegDef(ST.Reg);
    if (Def && Def->getParent() == L.Body && MRI->hasOneNonDBGUse(ST.Reg) &&
        Ops.IsMul(*Def, T)) {
      Dead.push_back(Def);
    } else {
      T.A = ST.Reg;
      T.B = Register();
    }
    Terms.push_back(T);
  }
  return !Terms.empty();
}

/// Finds the tree of adds in the exit block that adds the values \p Leaves,
/// which nothing else reads, together. Sets \p Root to its result.
bool MipsR5900LoopAcc::findExitSum(
    const SimpleLoop &L, ArrayRef<Register> Leaves,
    function_ref<bool(const MachineInstr &)> IsCombine, Register &Root,
    SmallVectorImpl<MachineInstr *> &Tree) const {
  if (L.Exit->pred_size() != 1)
    return false;
  SmallVector<Register, 8> Frontier(Leaves);
  // The one reader of Reg outside the loop (the PHI reads a leaf in it).
  auto GetUser = [&](Register Reg) -> MachineInstr * {
    MachineInstr *User = nullptr;
    for (MachineInstr &U : MRI->use_nodbg_instructions(Reg)) {
      if (U.getParent() == L.Body)
        continue;
      if (User)
        return nullptr;
      User = &U;
    }
    return User;
  };
  bool Changed = true;
  while (Frontier.size() > 1 && Changed) {
    Changed = false;
    for (unsigned I = 0; I < Frontier.size() && !Changed; ++I) {
      MachineInstr *UP = GetUser(Frontier[I]);
      if (!UP || UP->getParent() != L.Exit || !IsCombine(*UP))
        return false;
      MachineInstr &U = *UP;
      Register Op0 = U.getOperand(1).getReg(), Op1 = U.getOperand(2).getReg();
      auto *It0 = find(Frontier, Op0), *It1 = find(Frontier, Op1);
      if (It0 == Frontier.end() || It1 == Frontier.end() || It0 == It1)
        continue;
      Tree.push_back(&U);
      Frontier.erase(It0 < It1 ? It1 : It0);
      Frontier.erase(It0 < It1 ? It0 : It1);
      Frontier.push_back(U.getOperand(0).getReg());
      Changed = true;
    }
  }
  if (Frontier.size() != 1)
    return false;
  Root = Frontier[0];
  return true;
}

/// Gives the loop a preheader and an exit block of its own, for the
/// instructions that move the sum into ACC and out of it.
bool MipsR5900LoopAcc::prepareEdges(SimpleLoop &L) {
  if (L.Preheader->succ_size() != 1) {
    MachineBasicBlock *NewPre = L.Preheader->SplitCriticalEdge(L.Body, *this);
    if (!NewPre)
      return false;
    L.Preheader = NewPre;
  }
  if (L.Exit->pred_size() != 1) {
    MachineBasicBlock *NewExit = L.Body->SplitCriticalEdge(L.Exit, *this);
    if (!NewExit)
      return false;
    L.Exit = NewExit;
  }
  return true;
}

/// Replaces the uses of \p Old outside the loop. They all follow the exit
/// block, which defines \p New.
void MipsR5900LoopAcc::replaceOutsideUses(const SimpleLoop &L, Register Old,
                                          Register New) {
  for (MachineOperand &MO : make_early_inc_range(MRI->use_nodbg_operands(Old)))
    if (MO.getParent()->getParent() != L.Body)
      MO.setReg(New);
}

/// Returns true if \p Reg is +0.0 or an integer zero.
bool MipsR5900LoopAcc::isZero(Register Reg) const {
  while (Reg.isVirtual()) {
    MachineInstr *Def = MRI->getVRegDef(Reg);
    if (!Def)
      return false;
    if (Def->isCopy()) {
      Reg = Def->getOperand(1).getReg();
      continue;
    }
    return Def->getOpcode() == Mips::MTC1 &&
           Def->getOperand(1).getReg() == Mips::R0;
  }
  return Reg == Mips::R0_128 || Reg == Mips::R0_64 || Reg == Mips::R0;
}

/// Chooses the sums of a loop that go into the one accumulator: all of them
/// if they are only added together after the loop, else the first one.
/// Merged sums other than the first one must start at zero.
static SmallVector<unsigned, 4>
chooseSums(ArrayRef<LoopSum> Sums, ArrayRef<bool> Reassoc, bool CanMerge,
           function_ref<bool(Register)> IsZero) {
  SmallVector<unsigned, 4> Chosen;
  if (CanMerge && all_of(Reassoc, [](bool R) { return R; }) &&
      all_of(drop_begin(Sums),
             [&](const LoopSum &S) { return IsZero(S.Init); }))
    for (unsigned I = 0; I < Sums.size(); ++I)
      Chosen.push_back(I);
  else
    Chosen.push_back(0);
  return Chosen;
}

//===----------------------------------------------------------------------===//
// FPU
//===----------------------------------------------------------------------===//

bool MipsR5900LoopAcc::convertFPU(SimpleLoop L) {
  if (usesReg(L, Mips::FPU_ACC))
    return false;
  SumOps Ops{[](const MachineInstr &MI, Register &A, Register &B, bool &Neg) {
               if (MI.getOpcode() != Mips::FADD_S &&
                   MI.getOpcode() != Mips::FSUB_S)
                 return false;
               A = MI.getOperand(1).getReg();
               B = MI.getOperand(2).getReg();
               Neg = MI.getOpcode() == Mips::FSUB_S;
               return true;
             },
             [](const MachineInstr &MI, Term &T) {
               if (MI.getOpcode() != Mips::FMUL_S)
                 return false;
               T.A = MI.getOperand(1).getReg();
               T.B = MI.getOperand(2).getReg();
               return true;
             }};

  struct Candidate {
    LoopSum S;
    SmallVector<Term, 4> Terms;
    SmallVector<MachineInstr *, 8> Dead;
    bool Reassoc;
  };
  SmallVector<Candidate, 4> Cands;
  for (const LoopSum &S : getSums(L, &Mips::FGR32RegClass)) {
    Candidate C{S, {}, {}, false};
    bool NeedsReassoc;
    if (!collectTerms(L, S, Ops, C.Terms, C.Dead, NeedsReassoc))
      continue;
    C.Reassoc = all_of(C.Dead, [](MachineInstr *MI) {
      return MI->getOpcode() == Mips::FMUL_S ||
             MI->getFlag(MachineInstr::FmReassoc);
    });
    Cands.push_back(std::move(C));
  }
  if (Cands.empty())
    return false;

  SmallVector<LoopSum, 4> Sums;
  SmallVector<bool, 4> Reassoc;
  SmallVector<Register, 4> Leaves;
  for (const Candidate &C : Cands) {
    Sums.push_back(C.S);
    Reassoc.push_back(C.Reassoc);
    Leaves.push_back(C.S.Next);
  }
  Register Root;
  SmallVector<MachineInstr *, 4> Tree;
  bool CanMerge =
      Cands.size() > 1 && findExitSum(
                              L, Leaves,
                              [](const MachineInstr &MI) {
                                return MI.getOpcode() == Mips::FADD_S &&
                                       MI.getFlag(MachineInstr::FmReassoc);
                              },
                              Root, Tree);
  SmallVector<unsigned, 4> Chosen = chooseSums(
      Sums, Reassoc, CanMerge, [&](Register R) { return isZero(R); });
  if (Chosen.size() == 1) {
    Root = Cands[Chosen[0]].S.Next;
    Tree.clear();
  }

  if (!prepareEdges(L))
    return false;
  const DebugLoc DL;
  // 1.0 for the plain terms and the start value: exact in ft.
  MachineBasicBlock::iterator PreEnd = L.Preheader->getFirstTerminator();
  Register Init = Cands[Chosen[0]].S.Init;
  Register One;
  bool NeedsOne = !isZero(Init) || any_of(Chosen, [&](unsigned I) {
    return any_of(Cands[I].Terms, [](const Term &T) { return !T.B; });
  });
  if (NeedsOne) {
    Register Bits = MRI->createVirtualRegister(&Mips::GPR32RegClass);
    One = MRI->createVirtualRegister(&Mips::FGR32RegClass);
    BuildMI(*L.Preheader, PreEnd, DL, TII->get(Mips::LUi), Bits).addImm(0x3f80);
    BuildMI(*L.Preheader, PreEnd, DL, TII->get(Mips::MTC1), One).addReg(Bits);
  }
  // ACC = s0 * 1.0, or 0 * 0.
  BuildMI(*L.Preheader, PreEnd, DL, TII->get(Mips::MULA_S))
      .addReg(Init)
      .addReg(isZero(Init) ? Init : One);

  for (unsigned I : Chosen) {
    Candidate &C = Cands[I];
    MachineInstr *NextDef = MRI->getVRegDef(C.S.Next);
    for (const Term &T : C.Terms)
      BuildMI(*L.Body, NextDef, NextDef->getDebugLoc(),
              TII->get(T.Neg ? Mips::MSUBA_S : Mips::MADDA_S))
          .addReg(T.A)
          .addReg(T.B ? T.B : One);
  }

  // The sum: ACC - 0 * 0, which keeps a -0.0.
  MachineBasicBlock::iterator ExitStart = L.Exit->getFirstNonPHI();
  Register Zero = MRI->createVirtualRegister(&Mips::FGR32RegClass);
  Register Out = MRI->createVirtualRegister(&Mips::FGR32RegClass);
  BuildMI(*L.Exit, ExitStart, DL, TII->get(Mips::MTC1), Zero).addReg(Mips::R0);
  BuildMI(*L.Exit, ExitStart, DL, TII->get(Mips::R5900_MSUB_S), Out)
      .addReg(Zero)
      .addReg(Zero);
  replaceOutsideUses(L, Root, Out);
  for (MachineInstr *MI : reverse(Tree))
    MI->eraseFromParent();
  for (unsigned I : Chosen) {
    Candidate &C = Cands[I];
    for (MachineInstr *MI : C.Dead)
      MI->eraseFromParent();
    C.S.Phi->eraseFromParent();
  }
  L.Body->addLiveIn(Mips::FPU_ACC);
  L.Exit->addLiveIn(Mips::FPU_ACC);
  NumFPU += Chosen.size();
  LLVM_DEBUG(dbgs() << "R5900 loop ACC: " << Chosen.size() << " FPU sums in "
                    << printMBBReference(*L.Body) << "\n");
  return true;
}

//===----------------------------------------------------------------------===//
// VU0
//===----------------------------------------------------------------------===//

/// The VMADDA (Neg false) or VMSUBA form of a product with broadcast field
/// \p Field of ft, or all fields (-1).
static unsigned getVU0AccOpcode(int Field, bool Neg) {
  static const unsigned Add[] = {Mips::VMADDAbcx_V, Mips::VMADDAbcy_V,
                                 Mips::VMADDAbcz_V, Mips::VMADDAbcw_V};
  static const unsigned Sub[] = {Mips::VMSUBAbcx_V, Mips::VMSUBAbcy_V,
                                 Mips::VMSUBAbcz_V, Mips::VMSUBAbcw_V};
  if (Field < 0)
    return Neg ? Mips::VMSUBA_V : Mips::VMADDA_V;
  return Neg ? Sub[Field] : Add[Field];
}

/// The broadcast field of a VMULbc, or -1 for VMUL; -2 for other opcodes.
static int getVU0MulField(unsigned Opc) {
  switch (Opc) {
  case Mips::VMUL:
    return -1;
  case Mips::VMULbcx:
    return 0;
  case Mips::VMULbcy:
    return 1;
  case Mips::VMULbcz:
    return 2;
  case Mips::VMULbcw:
    return 3;
  default:
    return -2;
  }
}

bool MipsR5900LoopAcc::convertVU0(SimpleLoop L) {
  if (!STI->hasVU0() || usesReg(L, Mips::VU0ACC))
    return false;
  // ACC is one register: another ACC value in the loop would have to be
  // moved out of the way.
  for (const MachineInstr &MI : *L.Body)
    for (const MachineOperand &MO : MI.operands())
      if (MO.isReg() && MO.getReg().isVirtual() &&
          MRI->getRegClass(MO.getReg()) == &Mips::VU0ACCRCRegClass)
        return false;
  // All fields: dest mask xyzw.
  SumOps Ops{
      [](const MachineInstr &MI, Register &A, Register &B, bool &Neg) {
        if ((MI.getOpcode() != Mips::VADD && MI.getOpcode() != Mips::VSUB) ||
            MI.getOperand(1).getImm() != 15)
          return false;
        A = MI.getOperand(2).getReg();
        B = MI.getOperand(3).getReg();
        Neg = MI.getOpcode() == Mips::VSUB;
        return true;
      },
      [](const MachineInstr &MI, Term &T) {
        int Field = getVU0MulField(MI.getOpcode());
        if (Field == -2 || MI.getOperand(1).getImm() != 15)
          return false;
        T.A = MI.getOperand(2).getReg();
        T.B = MI.getOperand(3).getReg();
        T.Field = Field;
        return true;
      }};

  struct Candidate {
    LoopSum S;
    SmallVector<Term, 4> Terms;
    SmallVector<MachineInstr *, 8> Dead;
    bool Reassoc;
  };
  SmallVector<Candidate, 4> Cands;
  for (const LoopSum &S : getSums(L, &Mips::VFRegsRegClass)) {
    Candidate C{S, {}, {}, false};
    bool NeedsReassoc;
    if (!collectTerms(L, S, Ops, C.Terms, C.Dead, NeedsReassoc))
      continue;
    C.Reassoc = all_of(C.Dead, [](MachineInstr *MI) {
      return getVU0MulField(MI->getOpcode()) != -2 ||
             MI->getFlag(MachineInstr::FmReassoc);
    });
    Cands.push_back(std::move(C));
  }
  if (Cands.empty())
    return false;

  SmallVector<LoopSum, 4> Sums;
  SmallVector<bool, 4> Reassoc;
  SmallVector<Register, 4> Leaves;
  for (const Candidate &C : Cands) {
    Sums.push_back(C.S);
    Reassoc.push_back(C.Reassoc);
    Leaves.push_back(C.S.Next);
  }
  Register Root;
  SmallVector<MachineInstr *, 4> Tree;
  bool CanMerge =
      Cands.size() > 1 && findExitSum(
                              L, Leaves,
                              [](const MachineInstr &MI) {
                                return MI.getOpcode() == Mips::VADD &&
                                       MI.getOperand(1).getImm() == 15 &&
                                       MI.getFlag(MachineInstr::FmReassoc);
                              },
                              Root, Tree);
  // A zero vector is a load from the constant pool.
  auto IsZeroVec = [&](Register R) {
    MachineInstr *Def = MRI->getVRegDef(R);
    if (!Def || Def->getOpcode() != Mips::LQC2 || !Def->hasOneMemOperand())
      return false;
    const MachineMemOperand *MMO = *Def->memoperands_begin();
    const PseudoSourceValue *PSV = MMO->getPseudoValue();
    if (!PSV || !PSV->isConstantPool())
      return false;
    const MachineOperand &Off = Def->getOperand(2);
    if (!Off.isCPI())
      return false;
    const MachineConstantPoolEntry &E =
        Def->getMF()->getConstantPool()->getConstants()[Off.getIndex()];
    return !E.isMachineConstantPoolEntry() && E.Val.ConstVal->isNullValue();
  };
  SmallVector<unsigned, 4> Chosen =
      chooseSums(Sums, Reassoc, CanMerge, IsZeroVec);
  if (Chosen.size() == 1) {
    Root = Cands[Chosen[0]].S.Next;
    Tree.clear();
  }

  if (!prepareEdges(L))
    return false;
  const DebugLoc DL;
  MachineBasicBlock::iterator PreEnd = L.Preheader->getFirstTerminator();
  Register AccInit = MRI->createVirtualRegister(&Mips::VU0ACCRCRegClass);
  BuildMI(*L.Preheader, PreEnd, DL, TII->get(TargetOpcode::COPY), AccInit)
      .addReg(Cands[Chosen[0]].S.Init);
  Register AccPhi = MRI->createVirtualRegister(&Mips::VU0ACCRCRegClass);
  Register Acc = AccPhi;
  for (unsigned I : Chosen) {
    Candidate &C = Cands[I];
    MachineInstr *NextDef = MRI->getVRegDef(C.S.Next);
    for (const Term &T : C.Terms) {
      Register NewAcc = MRI->createVirtualRegister(&Mips::VU0ACCRCRegClass);
      // A plain term: t * vf0.w, 1.0 in ft.
      int Field = T.B ? T.Field : 3;
      BuildMI(*L.Body, NextDef, NextDef->getDebugLoc(),
              TII->get(getVU0AccOpcode(Field, T.Neg)), NewAcc)
          .addReg(Acc)
          .addImm(15)
          .addReg(T.A)
          .addReg(T.B ? T.B : Register(Mips::VF0));
      Acc = NewAcc;
    }
  }
  BuildMI(*L.Body, L.Body->begin(), DL, TII->get(TargetOpcode::PHI), AccPhi)
      .addReg(AccInit)
      .addMBB(L.Preheader)
      .addReg(Acc)
      .addMBB(L.Body);

  Register Out = MRI->createVirtualRegister(&Mips::VFRegsRegClass);
  BuildMI(*L.Exit, L.Exit->getFirstNonPHI(), DL, TII->get(TargetOpcode::COPY),
          Out)
      .addReg(Acc);
  replaceOutsideUses(L, Root, Out);
  for (MachineInstr *MI : reverse(Tree))
    MI->eraseFromParent();
  for (unsigned I : Chosen) {
    Candidate &C = Cands[I];
    for (MachineInstr *MI : C.Dead)
      MI->eraseFromParent();
    C.S.Phi->eraseFromParent();
  }
  NumVU0 += Chosen.size();
  LLVM_DEBUG(dbgs() << "R5900 loop ACC: " << Chosen.size() << " VU0 sums in "
                    << printMBBReference(*L.Body) << "\n");
  return true;
}

/// The ACC writing form of a VU0 ACC chain instruction: VMULA (no ACC read)
/// and VMADD (result in a VF register) to VMADDA, or 0.
static unsigned getVU0CarriedOpcode(unsigned Opc) {
  switch (Opc) {
  case Mips::VMULA_V:
  case Mips::VMADD_V:
    return Mips::VMADDA_V;
  case Mips::VMSUB_V:
    return Mips::VMSUBA_V;
  case Mips::VMULAbcx_V:
  case Mips::VMADDbcx_V:
    return Mips::VMADDAbcx_V;
  case Mips::VMULAbcy_V:
  case Mips::VMADDbcy_V:
    return Mips::VMADDAbcy_V;
  case Mips::VMULAbcz_V:
  case Mips::VMADDbcz_V:
    return Mips::VMADDAbcz_V;
  case Mips::VMULAbcw_V:
  case Mips::VMADDbcw_V:
    return Mips::VMADDAbcw_V;
  case Mips::VMSUBbcx_V:
    return Mips::VMSUBAbcx_V;
  case Mips::VMSUBbcy_V:
    return Mips::VMSUBAbcy_V;
  case Mips::VMSUBbcz_V:
    return Mips::VMSUBAbcz_V;
  case Mips::VMSUBbcw_V:
    return Mips::VMSUBAbcw_V;
  case Mips::VMADDA_V:
  case Mips::VMSUBA_V:
  case Mips::VMADDAbcx_V:
  case Mips::VMADDAbcy_V:
  case Mips::VMADDAbcz_V:
  case Mips::VMADDAbcw_V:
  case Mips::VMSUBAbcx_V:
  case Mips::VMSUBAbcy_V:
  case Mips::VMSUBAbcz_V:
  case Mips::VMSUBAbcw_V:
    return Opc;
  default:
    return 0;
  }
}

/// Returns true if \p Opc starts an ACC chain: it writes ACC without
/// reading it.
static bool isVU0AccStart(unsigned Opc) {
  return Opc == Mips::VMULA_V || Opc == Mips::VMULAbcx_V ||
         Opc == Mips::VMULAbcy_V || Opc == Mips::VMULAbcz_V ||
         Opc == Mips::VMULAbcw_V;
}

/// A sum that instruction selection already computes as an ACC chain with
/// the sum of the previous iteration as a plain term (s * vf0.w):
///   VMULA a, b; VMADDAw s, vf0; ...; s' = VMADD c, d
/// The chain then continues the ACC of the previous iteration:
///   VMADDA a, b; ...; VMADDA c, d
bool MipsR5900LoopAcc::convertVU0Chain(SimpleLoop L) {
  if (!STI->hasVU0() || usesReg(L, Mips::VU0ACC))
    return false;
  auto IsAccReg = [&](Register R) {
    return R.isVirtual() && MRI->getRegClass(R) == &Mips::VU0ACCRCRegClass;
  };
  for (const LoopSum &S : getSums(L, &Mips::VFRegsRegClass)) {
    MachineInstr *End = MRI->getVRegDef(S.Next);
    if (!getVU0CarriedOpcode(End->getOpcode()) ||
        End->getOperand(2).getImm() != 15)
      continue;
    // The chain, from its end to its start: each instruction reads the ACC
    // of the one before it, and nothing else reads that ACC.
    SmallVector<MachineInstr *, 8> Chain = {End};
    MachineInstr *PhiTerm = nullptr;
    Register PhiReg = S.Phi->getOperand(0).getReg();
    bool OK = true;
    while (OK) {
      MachineInstr *MI = Chain.back();
      unsigned Opc = MI->getOpcode();
      bool IsStart = isVU0AccStart(Opc);
      // Operands: (accd | fd), [accin], dest, fs, ft.
      unsigned FsIdx = IsStart ? 2 : 3;
      if (!getVU0CarriedOpcode(Opc) ||
          MI->getOperand(FsIdx - 1).getImm() != 15) {
        OK = false;
        break;
      }
      if (MI->getOperand(FsIdx).getReg() == PhiReg) {
        // s * vf0.w, once.
        if (PhiTerm || MI->getOperand(FsIdx + 1).getReg() != Mips::VF0 ||
            (Opc != Mips::VMADDAbcw_V && Opc != Mips::VMULAbcw_V &&
             Opc != Mips::VMADDbcw_V)) {
          OK = false;
          break;
        }
        PhiTerm = MI;
      }
      if (IsStart)
        break;
      Register AccIn = MI->getOperand(1).getReg();
      MachineInstr *Def = MRI->getVRegDef(AccIn);
      if (!Def || Def->getParent() != L.Body || !MRI->hasOneNonDBGUse(AccIn)) {
        OK = false;
        break;
      }
      Chain.push_back(Def);
    }
    // The chain has at least one product besides s * vf0.w.
    if (!OK || !PhiTerm || Chain.size() < 2)
      continue;
    // No other ACC value in the loop.
    SmallPtrSet<MachineInstr *, 8> InChain(Chain.begin(), Chain.end());
    if (any_of(*L.Body, [&](const MachineInstr &MI) {
          return !InChain.count(&MI) && any_of(MI.operands(), [&](auto &MO) {
            return MO.isReg() && IsAccReg(MO.getReg());
          });
        }))
      continue;

    if (!prepareEdges(L))
      return false;
    const DebugLoc DL;
    Register AccInit = MRI->createVirtualRegister(&Mips::VU0ACCRCRegClass);
    BuildMI(*L.Preheader, L.Preheader->getFirstTerminator(), DL,
            TII->get(TargetOpcode::COPY), AccInit)
        .addReg(S.Init);
    Register AccPhi = MRI->createVirtualRegister(&Mips::VU0ACCRCRegClass);
    // From the start: every instruction but the s * vf0.w term adds to the
    // ACC of the previous iteration.
    Register Acc = AccPhi;
    for (MachineInstr *MI : reverse(Chain)) {
      if (MI == PhiTerm) {
        MI->eraseFromParent();
        continue;
      }
      unsigned Opc = MI->getOpcode();
      bool IsStart = isVU0AccStart(Opc);
      unsigned FsIdx = IsStart ? 2 : 3;
      Register NewAcc = MRI->createVirtualRegister(&Mips::VU0ACCRCRegClass);
      BuildMI(*L.Body, MI, MI->getDebugLoc(),
              TII->get(getVU0CarriedOpcode(Opc)), NewAcc)
          .addReg(Acc)
          .add(MI->getOperand(FsIdx - 1))
          .add(MI->getOperand(FsIdx))
          .add(MI->getOperand(FsIdx + 1));
      Acc = NewAcc;
      MI->eraseFromParent();
    }
    BuildMI(*L.Body, L.Body->begin(), DL, TII->get(TargetOpcode::PHI), AccPhi)
        .addReg(AccInit)
        .addMBB(L.Preheader)
        .addReg(Acc)
        .addMBB(L.Body);
    Register Out = MRI->createVirtualRegister(&Mips::VFRegsRegClass);
    BuildMI(*L.Exit, L.Exit->getFirstNonPHI(), DL, TII->get(TargetOpcode::COPY),
            Out)
        .addReg(Acc);
    replaceOutsideUses(L, S.Next, Out);
    S.Phi->eraseFromParent();
    ++NumVU0;
    LLVM_DEBUG(dbgs() << "R5900 loop ACC: VU0 chain in "
                      << printMBBReference(*L.Body) << "\n");
    return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// MMI
//===----------------------------------------------------------------------===//

static const MCPhysReg HILORegs[] = {Mips::HI0_64, Mips::LO0_64, Mips::HI1_64,
                                     Mips::LO1_64};

/// The two halves of the v8i32 products of a PMULTH chain: A (0-3) =
/// PCPYLD(HI, LO) and B (4-7) = PCPYUD(LO, HI) after PMFLO and PMFHI.
struct MMIRead {
  MachineInstr *Start; // the PMULTH
  MachineInstr *Lo, *Hi;
  MachineInstr *A, *B;
};

bool MipsR5900LoopAcc::convertMMI(SimpleLoop L) {
  if (!STI->isR5900())
    return false;
  // Every instruction of the loop that uses HI or LO must be part of a
  // PMULTH chain that is read as two halves.
  SmallVector<MMIRead, 4> Reads;
  MachineInstr *Start = nullptr, *Lo = nullptr, *Hi = nullptr;
  auto UsesHILO = [&](const MachineInstr &MI) {
    return any_of(MI.operands(), [&](const MachineOperand &MO) {
      return MO.isReg() && MO.getReg().isPhysical() &&
             any_of(HILORegs, [&](MCPhysReg R) {
               return TRI->regsOverlap(MO.getReg(), R);
             });
    });
  };
  for (MachineInstr &MI : *L.Body) {
    if (!UsesHILO(MI))
      continue;
    switch (MI.getOpcode()) {
    case Mips::PMULTH:
      if (Start)
        return false;
      Start = &MI;
      break;
    case Mips::PMADDH:
      if (!Start || Lo || Hi)
        return false;
      break;
    case Mips::PMFLO:
      if (!Start || Lo)
        return false;
      Lo = &MI;
      break;
    case Mips::PMFHI:
      if (!Start || Hi)
        return false;
      Hi = &MI;
      break;
    default:
      return false;
    }
    if (Lo && Hi) {
      Reads.push_back({Start, Lo, Hi, nullptr, nullptr});
      Start = Lo = Hi = nullptr;
    }
  }
  if (Start || Reads.empty())
    return false;
  // The products that PMULTH and PMADDH also write to rd are not read.
  for (const MachineInstr &MI : *L.Body)
    if ((MI.getOpcode() == Mips::PMULTH || MI.getOpcode() == Mips::PMADDH) &&
        !MRI->use_nodbg_empty(MI.getOperand(0).getReg()))
      return false;

  // The halves.
  for (MMIRead &R : Reads) {
    Register LoReg = R.Lo->getOperand(0).getReg();
    Register HiReg = R.Hi->getOperand(0).getReg();
    for (MachineInstr &U : MRI->use_nodbg_instructions(LoReg)) {
      if (U.getOpcode() == Mips::PCPYLD && U.getOperand(1).getReg() == HiReg &&
          U.getOperand(2).getReg() == LoReg && !R.A)
        R.A = &U;
      else if (U.getOpcode() == Mips::PCPYUD &&
               U.getOperand(1).getReg() == LoReg &&
               U.getOperand(2).getReg() == HiReg && !R.B)
        R.B = &U;
      else
        return false;
    }
    if (!R.A || !R.B ||
        any_of(MRI->use_nodbg_instructions(HiReg),
               [&](MachineInstr &U) { return &U != R.A && &U != R.B; }))
      return false;
  }

  // Each half is a term of a v4i32 sum that the loop carries: all A halves
  // in sums of A halves, and all B halves in sums of B halves.
  SumOps Ops{[](const MachineInstr &MI, Register &A, Register &B, bool &Neg) {
               if (MI.getOpcode() != Mips::PADDW)
                 return false;
               A = MI.getOperand(1).getReg();
               B = MI.getOperand(2).getReg();
               Neg = false;
               return true;
             },
             [](const MachineInstr &, Term &) { return false; }, true};
  DenseMap<Register, bool> Half; // the result of a PCPYLD (A) or PCPYUD (B)
  for (const MMIRead &R : Reads) {
    Half[R.A->getOperand(0).getReg()] = false;
    Half[R.B->getOperand(0).getReg()] = true;
  }
  struct HalfSum {
    LoopSum S;
    SmallVector<MachineInstr *, 8> Dead;
  };
  SmallVector<HalfSum, 4> SumsOf[2];
  unsigned Found = 0;
  for (const LoopSum &S : getSums(L, &Mips::GPR128RegClass)) {
    SmallVector<Term, 4> Terms;
    HalfSum H{S, {}};
    bool NeedsReassoc;
    if (!collectTerms(L, S, Ops, Terms, H.Dead, NeedsReassoc))
      continue;
    int Side = -1;
    for (const Term &T : Terms) {
      auto It = Half.find(T.A);
      if (It == Half.end() || (Side >= 0 && Side != (int)It->second))
        return false;
      Side = It->second;
    }
    Found += Terms.size();
    SumsOf[Side].push_back(std::move(H));
  }
  if (Found != 2 * Reads.size() || SumsOf[0].empty() || SumsOf[1].empty())
    return false;

  // The sums of each half that are added together after the loop.
  Register Root[2];
  SmallVector<MachineInstr *, 4> Tree;
  Register Init[2];
  for (unsigned Side : {0u, 1u}) {
    SmallVector<Register, 4> Leaves;
    for (const HalfSum &H : SumsOf[Side])
      Leaves.push_back(H.S.Next);
    if (Leaves.size() == 1) {
      Root[Side] = Leaves[0];
    } else if (!findExitSum(
                   L, Leaves,
                   [](const MachineInstr &MI) {
                     return MI.getOpcode() == Mips::PADDW;
                   },
                   Root[Side], Tree) ||
               !all_of(drop_begin(SumsOf[Side]),
                       [&](const HalfSum &H) { return isZero(H.S.Init); })) {
      return false;
    }
    Init[Side] = SumsOf[Side][0].S.Init;
  }

  if (!prepareEdges(L))
    return false;
  const DebugLoc DL;
  // HI and LO hold the products 0, 1, 4, 5 (LO) and 2, 3, 6, 7 (HI):
  // LO = PCPYLD(B, A), HI = PCPYUD(A, B).
  MachineBasicBlock::iterator PreEnd = L.Preheader->getFirstTerminator();
  Register InitLo = Init[0], InitHi = Init[0];
  if (!isZero(Init[0]) || !isZero(Init[1])) {
    InitLo = MRI->createVirtualRegister(&Mips::GPR128RegClass);
    InitHi = MRI->createVirtualRegister(&Mips::GPR128RegClass);
    BuildMI(*L.Preheader, PreEnd, DL, TII->get(Mips::PCPYLD), InitLo)
        .addReg(Init[1])
        .addReg(Init[0]);
    BuildMI(*L.Preheader, PreEnd, DL, TII->get(Mips::PCPYUD), InitHi)
        .addReg(Init[0])
        .addReg(Init[1]);
  }
  Register AccInit = MRI->createVirtualRegister(&Mips::HILO128RCRegClass);
  BuildMI(*L.Preheader, PreEnd, DL, TII->get(Mips::PseudoPMTHILO), AccInit)
      .addReg(InitLo)
      .addReg(InitHi);

  // Every PMULTH and PMADDH adds its products to the sum.
  Register AccPhi = MRI->createVirtualRegister(&Mips::HILO128RCRegClass);
  Register Acc = AccPhi;
  for (MachineInstr &MI : make_early_inc_range(*L.Body)) {
    if (MI.getOpcode() != Mips::PMULTH && MI.getOpcode() != Mips::PMADDH)
      continue;
    Register NewAcc = MRI->createVirtualRegister(&Mips::HILO128RCRegClass);
    BuildMI(*L.Body, MI, MI.getDebugLoc(), TII->get(Mips::PseudoPMADDH))
        .addReg(MI.getOperand(0).getReg(), RegState::Define | RegState::Dead)
        .addReg(NewAcc, RegState::Define)
        .addReg(MI.getOperand(1).getReg())
        .addReg(MI.getOperand(2).getReg())
        .addReg(Acc);
    Acc = NewAcc;
    MI.eraseFromParent();
  }
  BuildMI(*L.Body, L.Body->begin(), DL, TII->get(TargetOpcode::PHI), AccPhi)
      .addReg(AccInit)
      .addMBB(L.Preheader)
      .addReg(Acc)
      .addMBB(L.Body);

  MachineBasicBlock::iterator ExitStart = L.Exit->getFirstNonPHI();
  Register OutLo = MRI->createVirtualRegister(&Mips::GPR128RegClass);
  Register OutHi = MRI->createVirtualRegister(&Mips::GPR128RegClass);
  Register OutA = MRI->createVirtualRegister(&Mips::GPR128RegClass);
  Register OutB = MRI->createVirtualRegister(&Mips::GPR128RegClass);
  BuildMI(*L.Exit, ExitStart, DL, TII->get(Mips::PseudoPMFLO), OutLo)
      .addReg(Acc);
  BuildMI(*L.Exit, ExitStart, DL, TII->get(Mips::PseudoPMFHI), OutHi)
      .addReg(Acc);
  BuildMI(*L.Exit, ExitStart, DL, TII->get(Mips::PCPYLD), OutA)
      .addReg(OutHi)
      .addReg(OutLo);
  BuildMI(*L.Exit, ExitStart, DL, TII->get(Mips::PCPYUD), OutB)
      .addReg(OutLo)
      .addReg(OutHi);
  replaceOutsideUses(L, Root[0], OutA);
  replaceOutsideUses(L, Root[1], OutB);
  for (MachineInstr *MI : reverse(Tree))
    MI->eraseFromParent();
  for (unsigned Side : {0u, 1u})
    for (HalfSum &H : SumsOf[Side]) {
      for (MachineInstr *MI : H.Dead)
        MI->eraseFromParent();
      H.S.Phi->eraseFromParent();
    }
  for (MMIRead &R : Reads)
    for (MachineInstr *MI : {R.A, R.B, R.Lo, R.Hi})
      MI->eraseFromParent();
  NumMMI += SumsOf[0].size() + SumsOf[1].size();
  LLVM_DEBUG(dbgs() << "R5900 loop ACC: " << Reads.size()
                    << " PMULTH chains in " << printMBBReference(*L.Body)
                    << "\n");
  return true;
}

bool MipsR5900LoopAcc::runOnMachineFunction(MachineFunction &MF) {
  STI = &MF.getSubtarget<MipsSubtarget>();
  if (!STI->isR5900() || DisableLoopAcc || skipFunction(MF.getFunction()))
    return false;
  TII = STI->getInstrInfo();
  TRI = STI->getRegisterInfo();
  MRI = &MF.getRegInfo();

  SmallVector<MachineBasicBlock *, 8> Loops;
  for (MachineBasicBlock &MBB : MF)
    if (getLoop(MBB))
      Loops.push_back(&MBB);
  bool Changed = false;
  for (MachineBasicBlock *MBB : Loops) {
    if (!STI->useSoftFloat())
      if (auto L = getLoop(*MBB))
        Changed |= convertFPU(*L);
    if (auto L = getLoop(*MBB))
      Changed |= convertVU0(*L) || convertVU0Chain(*L);
    if (auto L = getLoop(*MBB))
      Changed |= convertMMI(*L);
  }
  return Changed;
}
