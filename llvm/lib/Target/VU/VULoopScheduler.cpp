//===-- VULoopScheduler.cpp - Schedule the words of single block loops ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The schedulers and the packetizer only see one iteration of a loop, and the
// copies and moves that register allocation adds to a pipelined kernel are not
// in the schedule of the pipeliner. A word that waits for an operand stalls,
// also for an operand from the previous iteration.
//
// This pass schedules the instructions of a single block loop into L words
// that do not stall, with the branch in word L-2 and its delay slot in word
// L-1, for the smallest L. The dependences of one iteration and those from one
// iteration to the next are kept, the latter as a latency of L cycles less.
// It is a modulo schedule with a single stage, found by iterative modulo
// scheduling. The words are only replaced if they take fewer cycles.
//
// The registers are allocated already, so no instruction moves to another
// iteration. A register that holds several values orders them, which can
// rule out every schedule in L words, so the search is repeated with the
// values that live within one iteration renamed to registers that the loop
// does not use.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUMCTargetDesc.h"
#include "VU.h"
#include "VUSubtarget.h"
#include "VUWords.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/MC/MCSchedule.h"
#include "llvm/Support/MathExtras.h"
#include <functional>
#include <optional>

using namespace llvm;

#define DEBUG_TYPE "vu-loop-scheduler"

namespace {
class VULoopScheduler : public MachineFunctionPass {
public:
  static char ID;
  VULoopScheduler() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "VU Loop Scheduler"; }
  bool runOnMachineFunction(MachineFunction &MF) override;
};

/// Cycle(Dst) >= Cycle(Src) + Latency - Distance * L.
struct Edge {
  unsigned Src, Dst;
  int Latency;
  unsigned Distance;
};

/// The instructions of a loop and their dependences.
class LoopGraph {
  const TargetInstrInfo &TII;
  const TargetRegisterInfo &TRI;
  const MCSchedModel &SM;
  const TargetSubtargetInfo &STI;

public:
  SmallVector<MachineInstr *, 32> Instrs;
  /// The processor resources and cycles of each instruction.
  SmallVector<SmallVector<std::pair<unsigned, unsigned>, 2>, 32> Resources;
  SmallVector<Edge, 64> Edges;
  unsigned Branch = 0;
  /// A memory instruction whose base the loop only changes by an increment.
  /// It can be on either side of the increment, with the offset adjusted.
  struct Rebase {
    unsigned Mem, Inc;
    /// The base is the incremented one.
    bool After;
    int64_t Step;
  };
  SmallVector<Rebase, 4> Rebases;

  LoopGraph(const TargetSubtargetInfo &STI)
      : TII(*STI.getInstrInfo()), TRI(*STI.getRegisterInfo()),
        SM(STI.getSchedModel()), STI(STI) {}

  bool build(MachineBasicBlock &MBB);
  bool schedule(int L, SmallVectorImpl<int> &Cycles) const;

private:
  int getLatency(unsigned Def, Register Reg, unsigned Use,
                 const MachineOperand &UseMO) const;
};
} // namespace

char VULoopScheduler::ID = 0;

INITIALIZE_PASS(VULoopScheduler, DEBUG_TYPE, "VU Loop Scheduler", false, false)

FunctionPass *llvm::createVULoopSchedulerPass() {
  return new VULoopScheduler();
}

/// Returns the constant that \p MI adds to its VI operand, if it is an
/// increment of a register.
static std::optional<int64_t> getIncrement(const MachineInstr &MI) {
  int64_t Sign;
  switch (MI.getOpcode()) {
  case VU::IADDI:
  case VU::IADDIU:
    Sign = 1;
    break;
  case VU::ISUBIU:
    Sign = -1;
    break;
  default:
    return std::nullopt;
  }
  if (MI.getOperand(0).getReg() != MI.getOperand(1).getReg())
    return std::nullopt;
  return Sign * MI.getOperand(2).getImm();
}

int LoopGraph::getLatency(unsigned Def, Register Reg, unsigned Use,
                          const MachineOperand &UseMO) const {
  const MachineInstr &DefMI = *Instrs[Def];
  int DefIdx = DefMI.findRegisterDefOperandIdx(Reg, &TRI);
  if (DefIdx < 0)
    return 1;
  return TII
      .getOperandLatency(STI.getInstrItineraryData(), DefMI, DefIdx,
                         *Instrs[Use], UseMO.getOperandNo())
      .value_or(1);
}

/// Collects the instructions of the loop \p MBB, a sequence of words that ends
/// in the word of its branch and the delay slot, and their dependences.
bool LoopGraph::build(MachineBasicBlock &MBB) {
  // The words, in order. Both halves of a word read before they write.
  SmallVector<SmallVector<unsigned, 2>, 32> Words;
  bool Seen = false;
  for (MachineInstr &W : MBB) {
    if (W.isMetaInstruction())
      continue;
    SmallVector<unsigned, 2> Word;
    for (MachineInstr *MI : VU::getWordInstrs(W)) {
      unsigned Opc = MI->getOpcode();
      if (Opc == VU::NOP || Opc == VU::DSNOP || Opc == VU::DELAY_SLOT)
        continue;
      if (MI->isPseudo() || MI->isInlineAsm() || MI->isCall() ||
          MI->hasUnmodeledSideEffects())
        return false;
      if (MI->isBranch()) {
        if (Seen || !MI->isConditionalBranch())
          return false;
        Seen = true;
        Branch = Instrs.size();
      }
      Word.push_back(Instrs.size());
      Instrs.push_back(MI);
    }
    Words.push_back(Word);
  }
  if (!Seen)
    return false;

  // The increments of registers that the loop changes nowhere else.
  DenseMap<Register, unsigned> NumDefs;
  for (MachineInstr *MI : Instrs)
    for (const MachineOperand &MO : MI->all_defs())
      ++NumDefs[MO.getReg()];
  DenseMap<Register, unsigned> Increments;
  for (unsigned N = 0, E = Instrs.size(); N != E; ++N)
    if (getIncrement(*Instrs[N]) &&
        NumDefs[Instrs[N]->getOperand(0).getReg()] == 1)
      Increments[Instrs[N]->getOperand(0).getReg()] = N;
  // The memory instructions on them, and their offsets from the base at the
  // start of the iteration. Their dependences on the increment through the
  // base are left out.
  DenseMap<unsigned, unsigned> WordOf;
  for (unsigned W = 0, E = Words.size(); W != E; ++W)
    for (unsigned N : Words[W])
      WordOf[N] = W;
  SmallPtrSet<const MachineOperand *, 4> FreeBases;
  DenseMap<unsigned, int64_t> StartOffset;
  for (unsigned N = 0, E = Instrs.size(); N != E; ++N) {
    unsigned BasePos, OffsetPos;
    if (!TII.getBaseAndOffsetPosition(*Instrs[N], BasePos, OffsetPos))
      continue;
    const MachineOperand &Base = Instrs[N]->getOperand(BasePos);
    int64_t Offset = Instrs[N]->getOperand(OffsetPos).getImm();
    auto It = Increments.find(Base.getReg());
    if (It == Increments.end()) {
      if (!NumDefs.count(Base.getReg()))
        StartOffset[N] = Offset;
      continue;
    }
    unsigned Inc = It->second;
    int64_t Step = *getIncrement(*Instrs[Inc]);
    bool After = WordOf[N] > WordOf[Inc];
    StartOffset[N] = After ? Offset + Step : Offset;
    if (!isInt<11>(Offset + Step) || !isInt<11>(Offset - Step))
      continue;
    FreeBases.insert(&Base);
    Rebases.push_back({N, Inc, After, Step});
  }

  // Register dependences.
  DenseMap<Register, unsigned> LastDef;
  DenseMap<Register, SmallVector<unsigned, 4>> Readers, UpwardUses;
  SmallVector<unsigned, 8> MemOps;
  for (ArrayRef<unsigned> Word : Words) {
    for (unsigned N : Word)
      for (const MachineOperand &MO : Instrs[N]->all_uses()) {
        Register Reg = MO.getReg();
        if (!Reg || Reg == VU::VF0 || Reg == VU::VI0 || FreeBases.count(&MO))
          continue;
        auto It = LastDef.find(Reg);
        if (It != LastDef.end())
          Edges.push_back(
              {It->second, N, getLatency(It->second, Reg, N, MO), 0});
        else
          UpwardUses[Reg].push_back(N);
        Readers[Reg].push_back(N);
      }
    for (unsigned N : Word) {
      for (const MachineOperand &MO : Instrs[N]->all_defs()) {
        Register Reg = MO.getReg();
        // Reads of the old value come before, or in the same word.
        for (unsigned R : Readers[Reg])
          if (R != N)
            Edges.push_back({R, N, 0, 0});
        Readers[Reg].clear();
        auto It = LastDef.find(Reg);
        if (It != LastDef.end() && It->second != N)
          Edges.push_back({It->second, N, 1, 0});
        LastDef[Reg] = N;
      }
      const MachineInstr &MI = *Instrs[N];
      if (MI.mayLoadOrStore()) {
        // Quadwords of one base are compared by their offsets from the base
        // at the start of the iteration. Other bases may be anywhere.
        auto Disjoint = [&](unsigned M) {
          unsigned BasePosM, OffsetPosM, BasePosN, OffsetPosN;
          return StartOffset.count(M) && StartOffset.count(N) &&
                 TII.getBaseAndOffsetPosition(*Instrs[M], BasePosM,
                                              OffsetPosM) &&
                 TII.getBaseAndOffsetPosition(MI, BasePosN, OffsetPosN) &&
                 Instrs[M]->getOperand(BasePosM).getReg() ==
                     MI.getOperand(BasePosN).getReg() &&
                 StartOffset[M] != StartOffset[N];
        };
        for (unsigned M : MemOps) {
          const MachineInstr &Other = *Instrs[M];
          if ((MI.mayStore() || Other.mayStore()) && !Disjoint(M))
            Edges.push_back({M, N, 1, 0});
        }
        MemOps.push_back(N);
      }
    }
  }
  // From the last definition in one iteration to the uses in the next.
  for (auto &[Reg, Uses] : UpwardUses) {
    auto It = LastDef.find(Reg);
    if (It == LastDef.end())
      continue;
    for (unsigned U : Uses)
      for (const MachineOperand &MO : Instrs[U]->all_uses())
        if (MO.getReg() == Reg)
          Edges.push_back(
              {It->second, U, getLatency(It->second, Reg, U, MO), 1});
  }

  for (MachineInstr *MI : Instrs) {
    auto &R = Resources.emplace_back();
    const MCSchedClassDesc *SC =
        SM.getSchedClassDesc(MI->getDesc().getSchedClass());
    if (!SC || !SC->isValid())
      return false;
    for (const MCWriteProcResEntry &PRE :
         make_range(STI.getWriteProcResBegin(SC), STI.getWriteProcResEnd(SC)))
      R.push_back({PRE.ProcResourceIdx, PRE.ReleaseAtCycle});
  }
  return true;
}

/// Narrows the cycle windows \p Release and \p Deadline until every edge
/// holds for them. Returns false if a window becomes empty.
static bool propagate(ArrayRef<Edge> Edges, int L, std::vector<int> &Release,
                      std::vector<int> &Deadline) {
  for (unsigned Iter = 0, N = Release.size(); Iter <= N; ++Iter) {
    bool Changed = false;
    for (const Edge &E : Edges) {
      int Latency = E.Latency - (int)E.Distance * L;
      if (Release[E.Src] + Latency > Release[E.Dst]) {
        Release[E.Dst] = Release[E.Src] + Latency;
        Changed = true;
      }
      if (Deadline[E.Dst] - Latency < Deadline[E.Src]) {
        Deadline[E.Src] = Deadline[E.Dst] - Latency;
        Changed = true;
      }
      if (Release[E.Dst] > Deadline[E.Dst] || Release[E.Src] > Deadline[E.Src])
        return false;
    }
    if (!Changed)
      return true;
  }
  // A cycle of dependences that needs more than L words.
  return false;
}

/// Schedules the instructions into \p L words with the branch in word L-2,
/// by a search over the cycles of the instructions with the fewest choices
/// first. Returns false if there is no such schedule, or if the search takes
/// too long.
bool LoopGraph::schedule(int L, SmallVectorImpl<int> &Cycles) const {
  unsigned N = Instrs.size();
  std::vector<int> Release(N, 0), Deadline(N, L - 1);
  Release[Branch] = Deadline[Branch] = L - 2;
  if (!propagate(Edges, L, Release, Deadline)) {
    LLVM_DEBUG(dbgs() << "  the dependences need more than " << L
                      << " words\n");
    return false;
  }

  // The units in use in each cycle, modulo L.
  std::vector<std::vector<unsigned>> Use(SM.getNumProcResourceKinds(),
                                         std::vector<unsigned>(L, 0));
  auto Fits = [&](unsigned I, int C) {
    for (auto [Idx, Busy] : Resources[I])
      for (unsigned K = 0; K != Busy; ++K)
        if (Use[Idx][(C + K) % L] >= SM.getProcResource(Idx)->NumUnits)
          return false;
    return true;
  };
  auto Reserve = [&](unsigned I, int C, int Delta) {
    for (auto [Idx, Busy] : Resources[I])
      for (unsigned K = 0; K != Busy; ++K)
        Use[Idx][(C + K) % L] += Delta;
  };

  std::vector<bool> Placed(N, false);
  // Narrows the windows of the instructions that are not placed to the
  // cycles where their resources are free, and the dependences to those
  // windows. Returns false if a window becomes empty.
  auto Narrow = [&]() {
    for (bool Changed = true; Changed;) {
      Changed = false;
      if (!propagate(Edges, L, Release, Deadline))
        return false;
      for (unsigned I = 0; I != N; ++I) {
        if (Placed[I])
          continue;
        int R = Release[I], D = Deadline[I];
        while (R <= D && !Fits(I, R))
          ++R;
        while (D >= R && !Fits(I, D))
          --D;
        if (R > D)
          return false;
        if (R != Release[I] || D != Deadline[I]) {
          Release[I] = R;
          Deadline[I] = D;
          Changed = true;
        }
      }
    }
    return true;
  };
  unsigned Steps = 0;
  std::function<bool(unsigned)> Search = [&](unsigned NumPlaced) -> bool {
    if (NumPlaced == N)
      return true;
    if (++Steps > 1000000)
      return false;
    unsigned Op = N;
    for (unsigned I = 0; I != N; ++I)
      if (!Placed[I] &&
          (Op == N || Deadline[I] - Release[I] < Deadline[Op] - Release[Op] ||
           (Deadline[I] - Release[I] == Deadline[Op] - Release[Op] &&
            Release[I] < Release[Op])))
        Op = I;
    for (int C = Release[Op], E = Deadline[Op]; C <= E; ++C) {
      if (!Fits(Op, C))
        continue;
      std::vector<int> SavedRelease = Release, SavedDeadline = Deadline;
      Release[Op] = Deadline[Op] = C;
      Placed[Op] = true;
      Reserve(Op, C, 1);
      if (Narrow() && Search(NumPlaced + 1))
        return true;
      Reserve(Op, C, -1);
      Placed[Op] = false;
      Release = std::move(SavedRelease);
      Deadline = std::move(SavedDeadline);
      if (Steps > 1000000)
        break;
    }
    return false;
  };

  if (!Fits(Branch, L - 2))
    return false;
  Placed[Branch] = true;
  Reserve(Branch, L - 2, 1);
  if (!Narrow() || !Search(1)) {
    LLVM_DEBUG(dbgs() << "  no schedule found in " << Steps << " steps\n");
    return false;
  }
  LLVM_DEBUG(dbgs() << "  schedule in " << L << " words found in " << Steps
                    << " steps\n");
  Cycles.assign(Release.begin(), Release.end());
  return true;
}

/// A register operand renamed from \p Old.
struct Rename {
  MachineOperand *MO;
  Register Old;
};

/// Renames the values of VF and VI registers in the loop \p MBB that are
/// defined and used within one iteration, to registers that the loop does not
/// use. No instruction needs a particular VF or VI register, apart from the
/// reserved vf0 and vi0. Returns the renamed operands.
static SmallVector<Rename, 16>
renameLocalValues(MachineBasicBlock &MBB, const TargetRegisterInfo &TRI) {
  const MachineRegisterInfo &MRI = MBB.getParent()->getRegInfo();
  // The instructions and their words, in order.
  SmallVector<MachineInstr *, 32> Instrs;
  SmallVector<unsigned, 32> WordOf;
  unsigned NumWords = 0;
  for (MachineInstr &W : MBB)
    if (!W.isMetaInstruction()) {
      for (MachineInstr *MI : VU::getWordInstrs(W)) {
        Instrs.push_back(MI);
        WordOf.push_back(NumWords);
      }
      ++NumWords;
    }

  auto IsLive = [&](MCRegister Reg, const MachineBasicBlock &B) {
    return any_of(B.liveins(),
                  [&](const MachineBasicBlock::RegisterMaskPair &P) {
                    return TRI.regsOverlap(P.PhysReg, Reg);
                  });
  };
  auto IsLiveOut = [&](MCRegister Reg) {
    return any_of(MBB.successors(), [&](const MachineBasicBlock *Succ) {
      return IsLive(Reg, *Succ);
    });
  };
  auto IsUsed = [&](MCRegister Reg) {
    return any_of(Instrs, [&](const MachineInstr *MI) {
      return any_of(MI->operands(), [&](const MachineOperand &MO) {
        return MO.isReg() && MO.getReg() && TRI.regsOverlap(MO.getReg(), Reg);
      });
    });
  };
  // Free registers of each class.
  SmallVector<MCRegister, 16> Free[2];
  const TargetRegisterClass *Classes[] = {&VU::VFRegClass, &VU::VIRegClass};
  for (unsigned C = 0; C != 2; ++C)
    for (MCRegister Reg : *Classes[C])
      if (!MRI.isReserved(Reg) && !IsLive(Reg, MBB) && !IsLiveOut(Reg) &&
          !IsUsed(Reg))
        Free[C].push_back(Reg);

  SmallVector<Rename, 16> Renames;
  for (unsigned I = 0, E = Instrs.size(); I != E; ++I) {
    MachineInstr &Def = *Instrs[I];
    if (Def.getNumExplicitDefs() != 1 || !Def.getOperand(0).isReg())
      continue;
    MachineOperand &DefMO = Def.getOperand(0);
    Register Reg = DefMO.getReg();
    unsigned C;
    if (VU::VFRegClass.contains(Reg))
      C = 0;
    else if (VU::VIRegClass.contains(Reg))
      C = 1;
    else
      continue;
    // A partial write reads the register through a tied or implicit operand;
    // it continues the value before it.
    auto Continues = [&](const MachineOperand &MO) {
      return MO.isReg() && MO.getReg() && TRI.regsOverlap(MO.getReg(), Reg) &&
             MO.isUse() && (MO.isTied() || MO.isImplicit());
    };
    if (Free[C].empty() || MRI.isReserved(Reg) ||
        any_of(Def.operands(), Continues) ||
        any_of(Def.operands(), [&](const MachineOperand &MO) {
          return MO.isReg() && MO.isDef() && &MO != &DefMO &&
                 TRI.regsOverlap(MO.getReg(), Reg);
        }))
      continue;
    // The uses from the next word up to the word of the next definition,
    // which reads the value too: both halves of a word read before they
    // write.
    SmallVector<MachineOperand *, 4> Uses;
    bool Redefined = false, Fixed = false;
    unsigned J = I + 1;
    while (J != E && WordOf[J] == WordOf[I])
      ++J;
    for (; J != E && !Fixed && !(Redefined && WordOf[J] != WordOf[J - 1]); ++J)
      for (MachineOperand &MO : Instrs[J]->operands()) {
        if (!MO.isReg() || !MO.getReg() || !TRI.regsOverlap(MO.getReg(), Reg))
          continue;
        if (MO.isDef())
          Redefined = true;
        else if (Continues(MO) || MO.getReg() != Reg)
          Fixed = true;
        else
          Uses.push_back(&MO);
      }
    // Without a later definition, the value must not be live out.
    if (Fixed || Uses.empty() || (!Redefined && IsLiveOut(Reg)))
      continue;
    MCRegister New = Free[C].pop_back_val();
    LLVM_DEBUG(dbgs() << "  rename " << printReg(Reg, &TRI) << " to "
                      << printReg(New, &TRI) << " in " << Def);
    Renames.push_back({&DefMO, Reg});
    DefMO.setReg(New);
    for (MachineOperand *MO : Uses) {
      Renames.push_back({MO, Reg});
      MO->setReg(New);
    }
  }
  return Renames;
}

bool VULoopScheduler::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;
  const TargetSubtargetInfo &STI = MF.getSubtarget();
  const TargetInstrInfo &TII = *STI.getInstrInfo();
  const TargetRegisterInfo &TRI = *STI.getRegisterInfo();
  const InstrItineraryData *Itin = STI.getInstrItineraryData();
  bool Changed = false;

  for (MachineBasicBlock &MBB : MF) {
    if (!MBB.isSuccessor(&MBB))
      continue;
    SmallVector<VU::Word, 32> Current;
    for (MachineInstr &W : MBB) {
      if (W.isMetaInstruction())
        continue;
      VU::Word Word;
      for (MachineInstr *MI : VU::getWordInstrs(W))
        if (MI->getOpcode() != VU::DELAY_SLOT)
          (VU::isLowerInstr(MI->getDesc()) ? Word.Lower : Word.Upper) = MI;
      Current.push_back(Word);
    }
    int CurrentCycles = VU::getLoopCycles(Current, TII, Itin, TRI);

    // The fewest words that take fewer cycles than MaxCycles.
    struct Layout {
      SmallVector<VU::Word, 32> Words;
      int Cycles;
      DebugLoc DL;
      /// The offset operands that change, for the other side of an increment.
      SmallVector<std::pair<MachineOperand *, int64_t>, 4> Offsets;
    };
    auto FindLayout = [&](int MaxCycles) -> std::optional<Layout> {
      LoopGraph G(STI);
      if (!G.build(MBB)) {
        LLVM_DEBUG(dbgs() << printMBBReference(MBB) << ": not a simple loop\n");
        return std::nullopt;
      }

      // The resources of an iteration bound the number of words.
      const MCSchedModel &SM = STI.getSchedModel();
      std::vector<unsigned> Busy(SM.getNumProcResourceKinds(), 0);
      for (const auto &R : G.Resources)
        for (auto [Idx, Cycles] : R)
          Busy[Idx] += Cycles;
      int MinL = 2;
      for (unsigned Idx = 1, E = Busy.size(); Idx != E; ++Idx)
        MinL = std::max<int>(
            MinL, divideCeil(Busy[Idx], SM.getProcResource(Idx)->NumUnits));

      LLVM_DEBUG(dbgs() << printMBBReference(MBB) << ": " << MaxCycles
                        << " cycles, at least " << MinL << " words\n");
      SmallVector<int, 32> Cycles;
      for (int L = MinL; L < MaxCycles; ++L) {
        if (!G.schedule(L, Cycles)) {
          LLVM_DEBUG(dbgs() << "  no schedule in " << L << " words\n");
          continue;
        }
        Layout New{SmallVector<VU::Word, 32>(L), 0,
                   G.Instrs[G.Branch]->getDebugLoc()};
        New.Words[L - 1].DelaySlot = true;
        for (unsigned I = 0, E = G.Instrs.size(); I != E; ++I) {
          MachineInstr *MI = G.Instrs[I];
          (VU::isLowerInstr(MI->getDesc()) ? New.Words[Cycles[I]].Lower
                                           : New.Words[Cycles[I]].Upper) = MI;
        }
        for (const LoopGraph::Rebase &R : G.Rebases) {
          bool After = Cycles[R.Mem] > Cycles[R.Inc];
          if (After == R.After)
            continue;
          MachineInstr &MI = *G.Instrs[R.Mem];
          unsigned BasePos, OffsetPos;
          TII.getBaseAndOffsetPosition(MI, BasePos, OffsetPos);
          MachineOperand &Offset = MI.getOperand(OffsetPos);
          New.Offsets.push_back(
              {&Offset, Offset.getImm() + (After ? -R.Step : R.Step)});
        }
        New.Cycles = VU::getLoopCycles(New.Words, TII, Itin, TRI);
        LLVM_DEBUG(dbgs() << printMBBReference(MBB) << ": " << MaxCycles
                          << " cycles, " << L << " words take " << New.Cycles
                          << "\n");
        if (New.Cycles >= MaxCycles)
          return std::nullopt;
        return New;
      }
      return std::nullopt;
    };

    // With the registers as they are, and with the values renamed, which is
    // kept if it takes fewer cycles.
    std::optional<Layout> Best = FindLayout(CurrentCycles);
    SmallVector<Rename, 16> Renames = renameLocalValues(MBB, TRI);
    std::optional<Layout> Renamed;
    if (!Renames.empty())
      Renamed = FindLayout(Best ? Best->Cycles : CurrentCycles);
    if (Renamed)
      Best = std::move(Renamed);
    else
      for (auto [MO, Old] : Renames)
        MO->setReg(Old);
    if (!Best)
      continue;

    // Remove the NOPs and delay slot markers, which are rebuilt.
    SmallVector<MachineInstr *, 4> Old;
    for (MachineInstr &MI : MBB.instrs())
      if (MI.getOpcode() == VU::NOP || MI.getOpcode() == VU::DSNOP ||
          MI.getOpcode() == VU::DELAY_SLOT)
        Old.push_back(&MI);
    for (auto [MO, Offset] : Best->Offsets)
      MO->setImm(Offset);
    VU::rebuildWords(MBB, Best->Words, TII, Best->DL);
    for (MachineInstr *MI : Old)
      MI->eraseFromParent();
    Changed = true;
  }
  return Changed;
}
