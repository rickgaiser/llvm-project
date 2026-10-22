//===-- VULoopCopies.cpp - Avoid register copies in loops -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A software pipelined loop keeps a value for every stage in which it is
// used. Each one needs its own register and a copy at the end of the kernel.
// This pass removes two kinds of these copies before register allocation:
//
// - A pointer that is used in several stages is the same register at a
//   constant offset. A phi that is known to be another phi plus a constant on
//   every edge is replaced in memory instructions by the other phi, with the
//   offset added to the immediate. In the kernel, the latest register in the
//   increment chain of the pointer is used, so the old and the incremented
//   pointer are not live at the same time:
//     %p = PHI %a, %b ; %q = PHI %c, %p ; %b = IADDI %p, 3 ; SQ 0(%q)
//   ->
//     %p = PHI %a, %b ; %b = IADDI %p, 3 ; SQ -6(%b)
//
// - A value for the next iteration that is defined before the last use of
//   the value of this iteration overlaps it. The definition is moved below
//   that use when nothing in between depends on it.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUMCTargetDesc.h"
#include "VU.h"
#include "VUSubtarget.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"

using namespace llvm;

#define DEBUG_TYPE "vu-loop-copies"

namespace {
/// A register plus a constant, in quadwords.
struct RegOffset {
  Register Reg;
  int64_t Offset = 0;
};

class VULoopCopies : public MachineFunctionPass {
  const TargetInstrInfo *TII = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  MachineRegisterInfo *MRI = nullptr;
  /// Phis that are another phi in the same block plus a constant.
  DenseMap<Register, RegOffset> PhiOffsets;

  RegOffset resolve(Register Reg) const;
  bool isPhiOffset(MachineInstr &Q, MachineInstr &P, int64_t &Offset);
  void findPhiOffsets(MachineBasicBlock &MBB);
  bool mayAlias(const MachineInstr &A, const MachineInstr &B) const;
  bool canMoveBelow(const MachineInstr &MI, const MachineInstr &Other) const;
  bool sinkLoopValues(MachineBasicBlock &MBB);
  Register buildAdd(MachineBasicBlock &MBB, MachineBasicBlock::iterator I,
                    Register Src, int64_t Imm);
  bool rebaseExitPhis(MachineBasicBlock &MBB);
  bool rewriteBases(MachineBasicBlock &MBB);

public:
  static char ID;
  VULoopCopies() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "VU Loop Copies"; }
  bool runOnMachineFunction(MachineFunction &MF) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};
} // namespace

char VULoopCopies::ID = 0;

INITIALIZE_PASS(VULoopCopies, DEBUG_TYPE, "VU Loop Copies", false, false)

FunctionPass *llvm::createVULoopCopiesPass() { return new VULoopCopies(); }

/// Returns the constant that \p MI adds to its VI operand, if it is an
/// increment.
static std::optional<int64_t> getIncrement(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
  case VU::IADDI:
  case VU::IADDIU:
    return MI.getOperand(2).getImm();
  case VU::ISUBIU:
    return -MI.getOperand(2).getImm();
  default:
    return std::nullopt;
  }
}

/// Returns the source of a copy or increment, or an invalid register.
static Register getSource(const MachineInstr &MI) {
  if (!MI.isCopy() && !getIncrement(MI))
    return Register();
  Register Src = MI.getOperand(1).getReg();
  return Src.isVirtual() ? Src : Register();
}

/// Returns the incoming value of \p Phi from \p Pred.
static MachineOperand *getIncoming(MachineInstr &Phi,
                                   const MachineBasicBlock *Pred) {
  for (unsigned I = 1, E = Phi.getNumOperands(); I != E; I += 2)
    if (Phi.getOperand(I + 1).getMBB() == Pred)
      return &Phi.getOperand(I);
  return nullptr;
}

/// Follows copies, increments and known phi offsets back to a root.
RegOffset VULoopCopies::resolve(Register Reg) const {
  RegOffset R{Reg, 0};
  while (R.Reg.isVirtual()) {
    MachineInstr *Def = MRI->getVRegDef(R.Reg);
    if (!Def)
      break;
    if (Register Src = getSource(*Def)) {
      R.Offset += getIncrement(*Def).value_or(0);
      R.Reg = Src;
      continue;
    }
    auto It = PhiOffsets.find(R.Reg);
    if (It == PhiOffsets.end())
      break;
    R.Offset += It->second.Offset;
    R.Reg = It->second.Reg;
  }
  return R;
}

/// Returns true if phi \p Q is phi \p P plus \p Offset on every edge.
bool VULoopCopies::isPhiOffset(MachineInstr &Q, MachineInstr &P,
                               int64_t &Offset) {
  Register QReg = Q.getOperand(0).getReg();
  // Find the offset on an edge that does not depend on Q, then check all
  // edges assuming it.
  std::optional<int64_t> Candidate;
  for (MachineBasicBlock *Pred : Q.getParent()->predecessors()) {
    RegOffset A = resolve(getIncoming(Q, Pred)->getReg());
    RegOffset B = resolve(getIncoming(P, Pred)->getReg());
    if (A.Reg && A.Reg == B.Reg && A.Reg != QReg) {
      Candidate = A.Offset - B.Offset;
      break;
    }
  }
  if (!Candidate)
    return false;
  PhiOffsets[QReg] = {P.getOperand(0).getReg(), *Candidate};
  for (MachineBasicBlock *Pred : Q.getParent()->predecessors()) {
    RegOffset A = resolve(getIncoming(Q, Pred)->getReg());
    RegOffset B = resolve(getIncoming(P, Pred)->getReg());
    if (!A.Reg || A.Reg != B.Reg || A.Offset - B.Offset != *Candidate) {
      PhiOffsets.erase(QReg);
      return false;
    }
  }
  Offset = *Candidate;
  return true;
}

void VULoopCopies::findPhiOffsets(MachineBasicBlock &MBB) {
  SmallVector<MachineInstr *, 8> Phis;
  for (MachineInstr &Phi : MBB.phis())
    if (MRI->getRegClass(Phi.getOperand(0).getReg()) == &VU::VIRegClass)
      Phis.push_back(&Phi);
  // A pointer that is incremented in a loop is kept as the root, unless it
  // is expressed by another such pointer.
  SmallPtrSet<MachineInstr *, 8> Inductions;
  for (MachineInstr *Phi : Phis)
    if (MachineOperand *MO = getIncoming(*Phi, &MBB))
      if (resolve(MO->getReg()).Reg == Phi->getOperand(0).getReg())
        Inductions.insert(Phi);
  // A phi can only be expressed by another phi that is not expressed itself.
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (MachineInstr *Q : Phis) {
      if (PhiOffsets.count(Q->getOperand(0).getReg()))
        continue;
      for (MachineInstr *P : Phis) {
        int64_t Offset;
        if (P == Q || PhiOffsets.count(P->getOperand(0).getReg()) ||
            (Inductions.count(Q) && !Inductions.count(P)) ||
            !isPhiOffset(*Q, *P, Offset))
          continue;
        LLVM_DEBUG(dbgs() << printReg(Q->getOperand(0).getReg()) << " = "
                          << printReg(P->getOperand(0).getReg()) << " + "
                          << Offset << '\n');
        Changed = true;
        break;
      }
    }
  }
}

/// Returns true unless \p A and \p B access different quadwords of the same
/// pointer.
bool VULoopCopies::mayAlias(const MachineInstr &A,
                            const MachineInstr &B) const {
  unsigned BasePosA, OffsetPosA, BasePosB, OffsetPosB;
  if (!TII->getBaseAndOffsetPosition(A, BasePosA, OffsetPosA) ||
      !TII->getBaseAndOffsetPosition(B, BasePosB, OffsetPosB))
    return true;
  RegOffset RA = resolve(A.getOperand(BasePosA).getReg());
  RegOffset RB = resolve(B.getOperand(BasePosB).getReg());
  return RA.Reg != RB.Reg || RA.Offset + A.getOperand(OffsetPosA).getImm() ==
                                 RB.Offset + B.getOperand(OffsetPosB).getImm();
}

/// Returns true if \p MI can be moved below \p Other.
bool VULoopCopies::canMoveBelow(const MachineInstr &MI,
                                const MachineInstr &Other) const {
  if (Other.isTerminator() || Other.hasUnmodeledSideEffects() || Other.isCall())
    return false;
  if (MI.mayLoad() && Other.mayStore() && mayAlias(MI, Other))
    return false;
  for (const MachineOperand &MO : MI.operands()) {
    if (!MO.isReg() || !MO.getReg())
      continue;
    if (MO.isDef() ? Other.readsRegister(MO.getReg(), TRI) ||
                         Other.modifiesRegister(MO.getReg(), TRI)
                   : Other.modifiesRegister(MO.getReg(), TRI))
      return false;
  }
  return true;
}

/// Moves the definition of the value for the next iteration of a single
/// block loop below the last use of the value for this iteration.
bool VULoopCopies::sinkLoopValues(MachineBasicBlock &MBB) {
  if (!MBB.isSuccessor(&MBB))
    return false;
  bool Changed = false;
  for (MachineInstr &Phi : MBB.phis()) {
    Register P = Phi.getOperand(0).getReg();
    // A phi that is also needed for the next iteration overlaps anyway.
    if (any_of(MRI->use_nodbg_instructions(P), [&](const MachineInstr &MI) {
          return MI.isPHI() && MI.getParent() == &MBB;
        }))
      continue;
    MachineInstr *Def = MRI->getVRegDef(getIncoming(Phi, &MBB)->getReg());
    // Pointer increments are handled by rewriteBases.
    if (!Def || Def->getParent() != &MBB || Def->isPHI() || getSource(*Def) ||
        Def->mayStore() || Def->hasUnmodeledSideEffects() ||
        Def->hasOrderedMemoryRef() || Def->getNumDefs() != 1)
      continue;

    MachineBasicBlock::iterator Last;
    bool Found = false;
    for (MachineBasicBlock::iterator I = std::next(Def->getIterator()),
                                     E = MBB.end();
         I != E; ++I)
      if (I->readsRegister(P, TRI)) {
        Last = I;
        Found = true;
      }
    if (!Found)
      continue;
    bool CanMove = true;
    for (MachineBasicBlock::iterator I = std::next(Def->getIterator()); CanMove;
         ++I) {
      CanMove = canMoveBelow(*Def, *I);
      if (I == Last)
        break;
    }
    if (!CanMove)
      continue;
    LLVM_DEBUG(dbgs() << "Sinking " << *Def);
    MBB.splice(std::next(Last), &MBB, Def);
    Changed = true;
  }
  return Changed;
}

/// Returns \p Src plus \p Imm, built before \p I, or an invalid register if
/// the constant is too large.
Register VULoopCopies::buildAdd(MachineBasicBlock &MBB,
                                MachineBasicBlock::iterator I, Register Src,
                                int64_t Imm) {
  if (Imm == 0)
    return Src;
  unsigned Opc;
  if (isInt<5>(Imm))
    Opc = VU::IADDI;
  else if (isUInt<15>(Imm))
    Opc = VU::IADDIU;
  else if (isUInt<15>(-Imm)) {
    Opc = VU::ISUBIU;
    Imm = -Imm;
  } else
    return Register();
  Register Dst = MRI->createVirtualRegister(&VU::VIRegClass);
  BuildMI(MBB, I, DebugLoc(), TII->get(Opc), Dst).addReg(Src).addImm(Imm);
  return Dst;
}

/// A pointer that leaves a loop is the incremented register, which is live
/// out anyway, instead of an older one, which would need a register of its
/// own in the loop. The phis of a pointer after a loop are replaced by one
/// phi of the incremented registers; the differences are added to the
/// offsets of memory instructions, or by increments outside the loop.
bool VULoopCopies::rebaseExitPhis(MachineBasicBlock &MBB) {
  if (MBB.isSuccessor(&MBB))
    return false;
  // Members are erased below, roots are not.
  SmallVector<MachineInstr *, 4> Roots;
  for (MachineInstr &Phi : MBB.phis()) {
    Register R = Phi.getOperand(0).getReg();
    if (MRI->getRegClass(R) == &VU::VIRegClass && !PhiOffsets.count(R))
      Roots.push_back(&Phi);
  }
  bool Changed = false;
  for (MachineInstr *RootPtr : Roots) {
    MachineInstr &Root = *RootPtr;
    Register R = Root.getOperand(0).getReg();

    // The difference between the incremented and the incoming register.
    std::optional<int64_t> Delta;
    for (MachineBasicBlock *Loop : MBB.predecessors()) {
      if (!Loop->isSuccessor(Loop))
        continue;
      RegOffset X = resolve(getIncoming(Root, Loop)->getReg());
      MachineInstr *P = MRI->getVRegDef(X.Reg);
      if (!P || !P->isPHI() || P->getParent() != Loop)
        continue;
      RegOffset Y = resolve(getIncoming(*P, Loop)->getReg());
      if (Y.Reg == X.Reg) {
        Delta = Y.Offset - X.Offset;
        break;
      }
    }
    if (!Delta)
      continue;

    // The phis expressed by Root and the uses that need a register.
    SmallVector<std::pair<MachineInstr *, int64_t>, 4> Members = {{&Root, 0}};
    for (MachineInstr &Q : MBB.phis()) {
      auto It = PhiOffsets.find(Q.getOperand(0).getReg());
      if (It != PhiOffsets.end() && It->second.Reg == R)
        Members.push_back({&Q, It->second.Offset});
    }
    bool Fits = true;
    for (auto [Q, Offset] : Members)
      for (MachineInstr &MI :
           MRI->use_nodbg_instructions(Q->getOperand(0).getReg())) {
        unsigned BasePos, OffsetPos;
        if (TII->getBaseAndOffsetPosition(MI, BasePos, OffsetPos) &&
            MI.getOperand(BasePos).getReg() == Q->getOperand(0).getReg())
          Fits &=
              isInt<11>(MI.getOperand(OffsetPos).getImm() + Offset - *Delta);
      }
    if (!Fits)
      continue;

    // Rewrite the incoming registers.
    SmallVector<std::pair<MachineOperand *, Register>, 4> Incoming;
    for (MachineBasicBlock *Pred : MBB.predecessors()) {
      MachineOperand *MO = getIncoming(Root, Pred);
      RegOffset X = resolve(MO->getReg());
      MachineInstr *P = MRI->getVRegDef(X.Reg);
      Register New;
      if (Pred->isSuccessor(Pred) && P && P->isPHI() &&
          P->getParent() == Pred) {
        Register Y = getIncoming(*P, Pred)->getReg();
        if (resolve(Y).Offset - X.Offset == *Delta)
          New = Y;
      }
      // Prefer a register that is passed to another phi anyway.
      for (MachineBasicBlock *Succ : Pred->successors())
        for (MachineInstr &Phi : Succ->phis()) {
          if (New || &Phi == &Root)
            continue;
          Register Y = getIncoming(Phi, Pred)->getReg();
          RegOffset RY = resolve(Y);
          if (RY.Reg == X.Reg && RY.Offset - X.Offset == *Delta)
            New = Y;
        }
      if (!New)
        New = buildAdd(*Pred, Pred->getFirstTerminator(), MO->getReg(), *Delta);
      if (!New)
        break;
      Incoming.push_back({MO, New});
    }
    if (Incoming.size() != MBB.pred_size())
      continue;
    for (auto [MO, New] : Incoming)
      MO->setReg(New);

    // Root is now Delta higher. Rewrite the uses of Root and its members.
    MachineBasicBlock::iterator InsertPt = MBB.getFirstNonPHI();
    for (auto [Q, Offset] : Members) {
      Register QReg = Q->getOperand(0).getReg();
      Register Other;
      SmallVector<MachineOperand *, 8> Uses(
          make_pointer_range(MRI->use_nodbg_operands(QReg)));
      for (MachineOperand *UseMO : Uses) {
        MachineOperand &MO = *UseMO;
        MachineInstr &MI = *MO.getParent();
        unsigned BasePos, OffsetPos;
        if (TII->getBaseAndOffsetPosition(MI, BasePos, OffsetPos) &&
            &MI.getOperand(BasePos) == &MO) {
          MachineOperand &Imm = MI.getOperand(OffsetPos);
          Imm.setImm(Imm.getImm() + Offset - *Delta);
          MO.setReg(R);
          continue;
        }
        if (!Other)
          Other = buildAdd(MBB, InsertPt, R, Offset - *Delta);
        assert(Other && "Pointer difference is too large");
        MO.setReg(Other);
      }
      if (Q != &Root) {
        PhiOffsets.erase(QReg);
        Q->eraseFromParent();
      }
    }
    Changed = true;
  }
  return Changed;
}

/// Uses the latest register of each pointer as the base of memory
/// instructions.
bool VULoopCopies::rewriteBases(MachineBasicBlock &MBB) {
  // The registers that lead from a root phi to its value for the next
  // iteration of this block, which are live after each other.
  DenseMap<Register, Register> ChainRoot;
  DenseMap<Register, Register> Latest;
  for (MachineInstr &Phi : MBB.phis()) {
    Register P = Phi.getOperand(0).getReg();
    if (MRI->getRegClass(P) != &VU::VIRegClass || PhiOffsets.count(P))
      continue;
    Latest[P] = P;
    MachineOperand *MO = getIncoming(Phi, &MBB);
    for (Register R = MO ? MO->getReg() : Register(); R && R != P;) {
      MachineInstr *Def = MRI->getVRegDef(R);
      if (!Def || Def->getParent() != &MBB)
        break;
      ChainRoot[R] = P;
      R = getSource(*Def);
    }
  }

  bool Changed = false;
  for (MachineInstr &MI : MBB) {
    if (MI.isPHI())
      continue;
    unsigned BasePos, OffsetPos;
    if (TII->getBaseAndOffsetPosition(MI, BasePos, OffsetPos)) {
      MachineOperand &Base = MI.getOperand(BasePos);
      MachineOperand &Offset = MI.getOperand(OffsetPos);
      RegOffset R = resolve(Base.getReg());
      auto It = Latest.find(R.Reg);
      if (It != Latest.end() && It->second != Base.getReg()) {
        int64_t NewOffset =
            Offset.getImm() + R.Offset - resolve(It->second).Offset;
        if (isInt<11>(NewOffset)) {
          LLVM_DEBUG(dbgs() << "Rebasing " << MI);
          Base.setReg(It->second);
          Offset.setImm(NewOffset);
          Changed = true;
        }
      }
    }
    for (const MachineOperand &MO : MI.defs()) {
      auto It = ChainRoot.find(MO.getReg());
      if (It != ChainRoot.end())
        Latest[It->second] = MO.getReg();
    }
  }
  return Changed;
}

bool VULoopCopies::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;

  TII = MF.getSubtarget().getInstrInfo();
  TRI = MF.getSubtarget().getRegisterInfo();
  MRI = &MF.getRegInfo();
  PhiOffsets.clear();

  ReversePostOrderTraversal<MachineFunction *> RPOT(&MF);
  for (MachineBasicBlock *MBB : RPOT)
    findPhiOffsets(*MBB);

  bool Changed = false;
  // Sinking one value can put it after the definition of another one. Two
  // values that each read the phi of the other can not both be fixed, so
  // the number of rounds is limited.
  for (MachineBasicBlock &MBB : MF)
    for (unsigned Round = 0; Round != 4 && sinkLoopValues(MBB); ++Round)
      Changed = true;
  for (MachineBasicBlock *MBB : RPOT)
    Changed |= rebaseExitPhis(*MBB);
  for (MachineBasicBlock &MBB : MF)
    Changed |= rewriteBases(MBB);

  // Remove the phis and copies that are no longer used.
  bool Erased = true;
  while (Erased) {
    Erased = false;
    for (MachineBasicBlock &MBB : MF)
      for (MachineInstr &MI : make_early_inc_range(MBB)) {
        if ((!MI.isPHI() && !getSource(MI)) || !MI.getOperand(0).isReg())
          continue;
        Register Reg = MI.getOperand(0).getReg();
        if (Reg.isVirtual() && MRI->use_nodbg_empty(Reg)) {
          PhiOffsets.erase(Reg);
          MI.eraseFromParent();
          Erased = Changed = true;
        }
      }
  }
  return Changed;
}
