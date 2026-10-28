//===-- VULoopScheduler.cpp - Schedule the words of loops and blocks ------===//
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
// An exact move of fields has a form in each pipe: MR32.<f> d, s (f = x, y
// or z) and ADD<f+1>.<f> d, vf0, s, MOVE.<m> d, s and ADD.<m> d, vf0, s (m
// without w; vf0.xyz is 0). The search places it in either, in a loop that
// tests no MAC or status flag, which only the upper form writes.
//
// The registers are allocated already, so no instruction moves to another
// iteration. A register that holds several values orders them, which can
// rule out every schedule in L words, so the search is repeated with the
// values that live within one iteration renamed to registers that the loop
// does not use.
//
// The other blocks, such as the prologue and the epilogue of a pipelined loop,
// are scheduled the same way, without the modulo: into the fewest words that
// do not stall, with the branch, if any, in word L-2. Their instructions wait
// for the results of the blocks before them, and a result that is read after
// the block is ready no later than before, so that no block after it waits
// longer. A VI register is ready no later after the last word, too: a branch
// that reads it in the next word does not wait for it. A block that tests a
// MAC or status flag is left as it is: the flags are not registers.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUMCTargetDesc.h"
#include "VU.h"
#include "VUInstrInfo.h"
#include "VUSubtarget.h"
#include "VUWords.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/MC/MCSchedule.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MathExtras.h"
#include <climits>
#include <functional>
#include <optional>

using namespace llvm;

#define DEBUG_TYPE "vu-loop-scheduler"

static cl::opt<bool>
    EnableBlocks("vu-schedule-blocks", cl::Hidden, cl::init(true),
                 cl::desc("Schedule the words of blocks that are not loops"));

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
  /// The processor resources and cycles of each instruction, and of its form
  /// in the other pipe (none if AltOpc is 0).
  SmallVector<SmallVector<std::pair<unsigned, unsigned>, 2>, 32> Resources,
      AltResources;
  SmallVector<unsigned, 32> AltOpc;
  SmallVector<Edge, 64> Edges;
  static constexpr unsigned NoBranch = ~0u;
  unsigned Branch = NoBranch;
  /// A single block loop, or a block that runs once from its first word to
  /// its last.
  bool IsLoop = true;
  /// Of a block that is not a loop, for the results of the blocks before it
  /// and those read after it: the first cycle of each instruction, its last
  /// cycle, and its last cycle in L words, L + LastFromEnd.
  SmallVector<int, 32> FirstCycle, LastCycle, LastFromEnd;
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

  bool build(MachineBasicBlock &MBB, bool Loop);
  /// The fewest words for the dependences of a block that is not a loop.
  int getMinLength() const;
  /// Sets the cycle of each instruction, and whether it takes its other form.
  bool schedule(int L, SmallVectorImpl<int> &Cycles,
                SmallVectorImpl<bool> &UseAlt, unsigned &Steps) const;

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

/// The opcode of the form of the exact field move \p MI in the other pipe,
/// or 0. The operands are D, the mask and S (MR32, MOVE), or D, the mask,
/// vf0 and S (ADD).
static unsigned getAltOpcode(const MachineInstr &MI) {
  static constexpr unsigned Add[] = {VU::ADDx, VU::ADDy, VU::ADDz, VU::ADDw};
  unsigned Opc = MI.getOpcode();
  if (MI.getNumExplicitOperands() < 3 || !MI.getOperand(1).isImm())
    return 0;
  unsigned Mask = MI.getOperand(1).getImm();
  if (Mask & 1) // w: vf0.w is 1
    return 0;
  auto Field = [&]() -> int {
    switch (Mask) {
    case 8:
      return 0;
    case 4:
      return 1;
    case 2:
      return 2;
    }
    return -1;
  };
  if (Opc == VU::MOVE || Opc == VU::MR32) {
    if (!MI.getOperand(2).isReg() || MI.getOperand(2).getReg() == VU::VF0)
      return 0;
    if (Opc == VU::MOVE)
      return VU::ADD;
    return Field() >= 0 ? Add[Field() + 1] : 0;
  }
  if (MI.getNumExplicitOperands() != 4 || !MI.getOperand(2).isReg() ||
      MI.getOperand(2).getReg() != VU::VF0)
    return 0;
  if (Opc == VU::ADD)
    return VU::MOVE;
  for (int F = 1; F != 4; ++F)
    if (Opc == Add[F] && Field() == F - 1)
      return VU::MR32;
  return 0;
}

/// Changes the exact field move \p MI to its form in the other pipe.
static void switchForm(MachineInstr &MI, const TargetInstrInfo &TII) {
  unsigned Alt = getAltOpcode(MI);
  assert(Alt && "not a field move");
  bool ToUpper = MI.getOpcode() == VU::MOVE || MI.getOpcode() == VU::MR32;
  MachineFunction &MF = *MI.getMF();
  if (ToUpper) {
    // Insert vf0 before s: rebuild the operand list.
    SmallVector<MachineOperand, 6> Ops(MI.operands());
    while (MI.getNumOperands())
      MI.removeOperand(MI.getNumOperands() - 1);
    MI.setDesc(TII.get(Alt));
    for (unsigned I = 0, E = Ops.size(); I != E; ++I) {
      if (I == 2)
        MI.addOperand(MF, MachineOperand::CreateReg(VU::VF0, false));
      MI.addOperand(MF, Ops[I]);
    }
  } else {
    MI.removeOperand(2);
    MI.setDesc(TII.get(Alt));
  }
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

/// Collects the instructions of \p MBB, a loop if \p Loop, and their
/// dependences. A loop is a sequence of words that ends in the word of its
/// conditional branch and the delay slot; another block may end in a branch
/// and its delay slot too.
bool LoopGraph::build(MachineBasicBlock &MBB, bool Loop) {
  IsLoop = Loop;
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
        if (Seen || (IsLoop && !MI->isConditionalBranch()))
          return false;
        Seen = true;
        Branch = Instrs.size();
      }
      Word.push_back(Instrs.size());
      Instrs.push_back(MI);
    }
    Words.push_back(Word);
  }
  if (IsLoop && !Seen)
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
    if (!IsLoop)
      break;
    auto It = LastDef.find(Reg);
    if (It == LastDef.end())
      continue;
    for (unsigned U : Uses)
      for (const MachineOperand &MO : Instrs[U]->all_uses())
        if (MO.getReg() == Reg)
          Edges.push_back(
              {It->second, U, getLatency(It->second, Reg, U, MO), 1});
  }

  auto GetResources = [&](unsigned Opc, auto &R) {
    const MCSchedClassDesc *SC =
        SM.getSchedClassDesc(TII.get(Opc).getSchedClass());
    if (!SC || !SC->isValid())
      return false;
    for (const MCWriteProcResEntry &PRE :
         make_range(STI.getWriteProcResBegin(SC), STI.getWriteProcResEnd(SC)))
      R.push_back({PRE.ProcResourceIdx, PRE.ReleaseAtCycle});
    return true;
  };
  bool TestsFlags = any_of(Instrs, [](const MachineInstr *MI) {
    switch (MI->getOpcode()) {
    case VU::FMAND:
    case VU::FMEQ:
    case VU::FMOR:
    case VU::FSAND:
    case VU::FSEQ:
    case VU::FSOR:
    case VU::FSSET:
      return true;
    }
    return false;
  });
  if (TestsFlags && !IsLoop)
    return false;
  for (MachineInstr *MI : Instrs) {
    if (!GetResources(MI->getOpcode(), Resources.emplace_back()))
      return false;
    unsigned Alt = TestsFlags ? 0 : getAltOpcode(*MI);
    AltOpc.push_back(Alt);
    auto &AR = AltResources.emplace_back();
    if (Alt && !GetResources(Alt, AR))
      return false;
  }
  FirstCycle.assign(Instrs.size(), 0);
  LastCycle.assign(Instrs.size(), INT_MAX);
  LastFromEnd.assign(Instrs.size(), INT_MAX);
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

int LoopGraph::getMinLength() const {
  std::vector<int> Release(FirstCycle.begin(), FirstCycle.end()),
      Deadline(Instrs.size(), INT_MAX / 2);
  propagate(Edges, 0, Release, Deadline);
  int L = 1;
  for (unsigned I = 0, E = Instrs.size(); I != E; ++I) {
    L = std::max(L, Release[I] + 1);
    if (LastFromEnd[I] != INT_MAX)
      L = std::max(L, Release[I] - LastFromEnd[I]);
  }
  if (Branch != NoBranch)
    L = std::max(L, Release[Branch] + 2);
  return L;
}

/// Schedules the instructions into \p L words with the branch, if any, in word
/// L-2, by a search over the cycles of the instructions with the fewest
/// choices first. The cycles of a loop are modulo L. Returns false if there is
/// no such schedule, or if the search takes more than \p Steps steps, which are
/// taken off.
bool LoopGraph::schedule(int L, SmallVectorImpl<int> &Cycles,
                         SmallVectorImpl<bool> &UseAlt, unsigned &Steps) const {
  unsigned N = Instrs.size();
  std::vector<int> Release(N, 0), Deadline(N, L - 1);
  if (!IsLoop)
    for (unsigned I = 0; I != N; ++I) {
      Release[I] = FirstCycle[I];
      Deadline[I] = std::min(Deadline[I], LastCycle[I]);
      if (LastFromEnd[I] != INT_MAX)
        Deadline[I] = std::min(Deadline[I], L + LastFromEnd[I]);
      if (Release[I] > Deadline[I])
        return false;
    }
  if (Branch != NoBranch)
    Release[Branch] = Deadline[Branch] = L - 2;
  if (!propagate(Edges, L, Release, Deadline)) {
    LLVM_DEBUG(dbgs() << "  the dependences need more than " << L
                      << " words\n");
    return false;
  }

  // The units in use in each cycle, modulo L in a loop. After the last word
  // of another block they are busy until they are done.
  unsigned Span = L;
  if (!IsLoop)
    for (unsigned I = 0; I != N; ++I)
      for (auto *R : {&Resources[I], &AltResources[I]})
        for (auto [Idx, Busy] : *R)
          Span = std::max<unsigned>(Span, L + Busy);
  auto Slot = [&](int C) { return IsLoop ? C % L : C; };
  std::vector<std::vector<unsigned>> Use(SM.getNumProcResourceKinds(),
                                         std::vector<unsigned>(Span, 0));
  std::vector<bool> Alt(N, false);
  auto ResourcesOf = [&](unsigned I,
                         bool A) -> ArrayRef<std::pair<unsigned, unsigned>> {
    return A ? AltResources[I] : Resources[I];
  };
  auto FitsForm = [&](unsigned I, int C, bool A) {
    for (auto [Idx, Busy] : ResourcesOf(I, A))
      for (unsigned K = 0; K != Busy; ++K)
        if (Use[Idx][Slot(C + K)] >= SM.getProcResource(Idx)->NumUnits)
          return false;
    return true;
  };
  // In either form.
  auto Fits = [&](unsigned I, int C) {
    return FitsForm(I, C, false) || (AltOpc[I] && FitsForm(I, C, true));
  };
  auto Reserve = [&](unsigned I, int C, int Delta) {
    for (auto [Idx, Busy] : ResourcesOf(I, Alt[I]))
      for (unsigned K = 0; K != Busy; ++K)
        Use[Idx][Slot(C + K)] += Delta;
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
  unsigned Taken = 0;
  std::function<bool(unsigned)> Search = [&](unsigned NumPlaced) -> bool {
    if (NumPlaced == N)
      return true;
    if (++Taken > Steps)
      return false;
    unsigned Op = N;
    for (unsigned I = 0; I != N; ++I)
      if (!Placed[I] &&
          (Op == N || Deadline[I] - Release[I] < Deadline[Op] - Release[Op] ||
           (Deadline[I] - Release[I] == Deadline[Op] - Release[Op] &&
            Release[I] < Release[Op])))
        Op = I;
    for (int C = Release[Op], E = Deadline[Op]; C <= E; ++C) {
      for (bool A : {false, true}) {
        if ((A && !AltOpc[Op]) || !FitsForm(Op, C, A))
          continue;
        std::vector<int> SavedRelease = Release, SavedDeadline = Deadline;
        Release[Op] = Deadline[Op] = C;
        Placed[Op] = true;
        Alt[Op] = A;
        Reserve(Op, C, 1);
        if (Narrow() && Search(NumPlaced + 1))
          return true;
        Reserve(Op, C, -1);
        Placed[Op] = false;
        Alt[Op] = false;
        Release = std::move(SavedRelease);
        Deadline = std::move(SavedDeadline);
        if (Taken > Steps)
          return false;
      }
    }
    return false;
  };

  unsigned NumPlaced = 0;
  if (Branch != NoBranch) {
    if (!FitsForm(Branch, L - 2, false))
      return false;
    Placed[Branch] = true;
    Reserve(Branch, L - 2, 1);
    NumPlaced = 1;
  }
  bool Found = Narrow() && Search(NumPlaced);
  Steps -= std::min(Steps, Taken);
  if (!Found) {
    LLVM_DEBUG(dbgs() << "  no schedule found in " << Taken << " steps\n");
    return false;
  }
  LLVM_DEBUG(dbgs() << "  schedule in " << L << " words found in " << Taken
                    << " steps\n");
  Cycles.assign(Release.begin(), Release.end());
  UseAlt.assign(Alt.begin(), Alt.end());
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

/// The last definition of each register and its cycle.
using DefMap = DenseMap<Register, std::pair<MachineInstr *, int>>;

/// Returns the words of \p MBB.
static SmallVector<VU::Word, 32> getWords(MachineBasicBlock &MBB) {
  SmallVector<VU::Word, 32> Words;
  for (MachineInstr &W : MBB) {
    if (W.isMetaInstruction())
      continue;
    VU::Word Word;
    for (MachineInstr *MI : VU::getWordInstrs(W))
      if (MI->getOpcode() != VU::DELAY_SLOT)
        (VU::isLowerInstr(MI->getDesc()) ? Word.Lower : Word.Upper) = MI;
    Words.push_back(Word);
  }
  return Words;
}

/// Returns the cycles of \p Words run once from cycle 0, stalls included.
/// \p Defs holds the definitions before the words, at cycles below 0, and is
/// updated with those of the words.
static int getBlockCycles(ArrayRef<VU::Word> Words, DefMap &Defs,
                          const TargetInstrInfo &TII,
                          const InstrItineraryData *Itin,
                          const TargetRegisterInfo &TRI) {
  int Cycle = -1;
  for (const VU::Word &W : Words) {
    int Issue = Cycle + 1;
    for (MachineInstr *MI : {W.Upper, W.Lower}) {
      if (!MI)
        continue;
      for (const MachineOperand &MO : MI->all_uses()) {
        auto It = Defs.find(MO.getReg());
        if (It == Defs.end())
          continue;
        auto [Def, DefCycle] = It->second;
        int DefIdx = Def->findRegisterDefOperandIdx(MO.getReg(), &TRI);
        if (DefIdx < 0)
          continue;
        Issue = std::max<int>(
            Issue, DefCycle + TII.getOperandLatency(Itin, *Def, DefIdx, *MI,
                                                    MO.getOperandNo())
                                  .value_or(1));
      }
    }
    Cycle = Issue;
    for (MachineInstr *MI : {W.Upper, W.Lower})
      if (MI)
        for (const MachineOperand &MO : MI->all_defs())
          Defs[MO.getReg()] = {MI, Cycle};
  }
  return Cycle + 1;
}

/// The cycles after the instruction \p MI until its result \p Reg can be read
/// by any instruction without a stall, also by a branch.
static int getReadyLatency(const MachineInstr &MI, Register Reg,
                           const TargetRegisterInfo &TRI) {
  int DefIdx = MI.findRegisterDefOperandIdx(Reg, &TRI);
  if (DefIdx < 0)
    return 1;
  int Latency = VUInstrInfo::getResultLatency(MI, DefIdx);
  return VU::VIRegClass.contains(Reg) ? std::max(Latency, 2) : Latency;
}

/// Adds to \p Defs the definitions of the blocks before \p MBB that it may
/// still wait for, at cycles before its start, which is \p Offset cycles
/// after the end of those blocks. A register in \p Shadow is defined later.
static void collectIncoming(MachineBasicBlock &MBB, int Offset, unsigned Depth,
                            const DenseSet<Register> &Shadow, DefMap &Defs,
                            const TargetInstrInfo &TII,
                            const InstrItineraryData *Itin,
                            const TargetRegisterInfo &TRI) {
  // Longer than the latencies that matter: the FDIV and most EFU results.
  constexpr int Horizon = 32;
  for (MachineBasicBlock *Pred : MBB.predecessors()) {
    DefMap PredDefs;
    int Cycles = getBlockCycles(getWords(*Pred), PredDefs, TII, Itin, TRI);
    DenseSet<Register> Later = Shadow;
    for (auto &[Reg, D] : PredDefs) {
      if (!Later.insert(Reg).second)
        continue;
      auto [MI, Cycle] = D;
      int At = Cycle - Cycles - Offset;
      int Ready = At + getReadyLatency(*MI, Reg, TRI);
      if (Ready <= 0)
        continue;
      auto It = Defs.find(Reg);
      if (It == Defs.end() ||
          It->second.second + getReadyLatency(*It->second.first, Reg, TRI) <
              Ready)
        Defs[Reg] = {MI, At};
    }
    if (Depth < 3 && Offset + Cycles < Horizon)
      collectIncoming(*Pred, Offset + Cycles, Depth + 1, Later, Defs, TII, Itin,
                      TRI);
  }
}

/// Removes the empty words of the layout \p Words of a block that is not a
/// loop while it takes no more than \p Cycles: the VU waits for an operand as
/// long as the empty words do. A branch does not wait for a VI register that
/// the word before it sets, so it stays as far from the definitions of its
/// operands, and from the start of the block if a block before it sets one.
/// (A read of Q or a test of the clip flags gets its wait from VUHazards.)
static void removeEmptyWords(SmallVectorImpl<VU::Word> &Words, int Cycles,
                             const DefMap &Incoming, const TargetInstrInfo &TII,
                             const InstrItineraryData *Itin,
                             const TargetRegisterInfo &TRI) {
  auto IsBranch = [](const VU::Word &W) {
    return (W.Upper && W.Upper->isBranch()) || (W.Lower && W.Lower->isBranch());
  };
  unsigned MinBranchWord = 0;
  for (unsigned B = 0, E = Words.size(); B != E; ++B)
    if (IsBranch(Words[B]))
      MinBranchWord = std::min(B, 2u);
  auto BranchWaits = [&](ArrayRef<VU::Word> Ws) {
    for (unsigned B = 0, E = Ws.size(); B != E; ++B) {
      if (!IsBranch(Ws[B]))
        continue;
      for (MachineInstr *MI : {Ws[B].Upper, Ws[B].Lower}) {
        if (!MI || !MI->isBranch())
          continue;
        for (const MachineOperand &MO : MI->all_uses()) {
          if (!VU::VIRegClass.contains(MO.getReg()) || MO.getReg() == VU::VI0)
            continue;
          bool Found = false;
          for (unsigned D = B; D-- > 0 && !Found;)
            for (MachineInstr *Def : {Ws[D].Upper, Ws[D].Lower}) {
              int Idx =
                  Def ? Def->findRegisterDefOperandIdx(MO.getReg(), &TRI) : -1;
              if (Idx < 0)
                continue;
              Found = true;
              if ((int)(B - D) < (int)TII
                                     .getOperandLatency(Itin, *Def, Idx, *MI,
                                                        MO.getOperandNo())
                                     .value_or(1))
                return false;
            }
          if (!Found && B < MinBranchWord)
            return false;
        }
      }
    }
    return true;
  };
  for (unsigned W = Words.size(); W-- > 0;) {
    if (Words[W].Upper || Words[W].Lower || Words[W].DelaySlot)
      continue;
    SmallVector<VU::Word, 32> Fewer(Words.begin(), Words.end());
    Fewer.erase(Fewer.begin() + W);
    DefMap Defs = Incoming;
    if (BranchWaits(Fewer) &&
        getBlockCycles(Fewer, Defs, TII, Itin, TRI) <= Cycles)
      Words.assign(Fewer.begin(), Fewer.end());
  }
}

static bool isLiveOut(const MachineBasicBlock &MBB, MCRegister Reg,
                      const TargetRegisterInfo &TRI) {
  return any_of(MBB.successors(), [&](const MachineBasicBlock *Succ) {
    return any_of(Succ->liveins(),
                  [&](const MachineBasicBlock::RegisterMaskPair &P) {
                    return TRI.regsOverlap(P.PhysReg, Reg);
                  });
  });
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
    bool IsLoop = MBB.isSuccessor(&MBB);
    if (!IsLoop && !EnableBlocks)
      continue;
    SmallVector<VU::Word, 32> Current = getWords(MBB);
    // Another block: the definitions before it that it may wait for, and
    // those of its words as they are.
    DefMap Incoming, CurrentDefs;
    int CurrentCycles;
    if (IsLoop) {
      CurrentCycles = VU::getLoopCycles(Current, TII, Itin, TRI);
    } else {
      if (count_if(Current, [](const VU::Word &W) {
            return (W.Upper && W.Upper->getOpcode() != VU::NOP) ||
                   (W.Lower && W.Lower->getOpcode() != VU::NOP);
          }) < 2)
        continue;
      collectIncoming(MBB, 0, 0, {}, Incoming, TII, Itin, TRI);
      CurrentDefs = Incoming;
      CurrentCycles = getBlockCycles(Current, CurrentDefs, TII, Itin, TRI);
    }
    LLVM_DEBUG(dbgs() << printMBBReference(MBB) << ": " << Current.size()
                      << " words take " << CurrentCycles << " cycles\n");

    // The fewest words that take fewer cycles than MaxCycles.
    struct Layout {
      SmallVector<VU::Word, 32> Words;
      int Cycles;
      DebugLoc DL;
      /// The offset operands that change, for the other side of an increment.
      SmallVector<std::pair<MachineOperand *, int64_t>, 4> Offsets;
      /// The field moves that change to the other pipe.
      SmallVector<MachineInstr *, 4> Switched;
    };
    auto FindLayout = [&](int MaxCycles) -> std::optional<Layout> {
      LoopGraph G(STI);
      if (!G.build(MBB, IsLoop)) {
        LLVM_DEBUG(dbgs() << printMBBReference(MBB) << ": not a simple "
                          << (IsLoop ? "loop" : "block") << "\n");
        return std::nullopt;
      }
      if (!IsLoop)
        for (unsigned I = 0, E = G.Instrs.size(); I != E; ++I) {
          MachineInstr &MI = *G.Instrs[I];
          for (const MachineOperand &MO : MI.all_uses()) {
            auto It = Incoming.find(MO.getReg());
            if (It == Incoming.end())
              continue;
            auto [Def, Cycle] = It->second;
            int DefIdx = Def->findRegisterDefOperandIdx(MO.getReg(), &TRI);
            if (DefIdx >= 0)
              G.FirstCycle[I] = std::max<int>(
                  G.FirstCycle[I],
                  Cycle + TII.getOperandLatency(Itin, *Def, DefIdx, MI,
                                                MO.getOperandNo())
                              .value_or(1));
          }
          // A result read after the block is ready no later, a VI register
          // no later after its end either.
          for (const MachineOperand &MO : MI.all_defs()) {
            auto It = CurrentDefs.find(MO.getReg());
            if (It == CurrentDefs.end() || It->second.first != &MI ||
                !isLiveOut(MBB, MO.getReg(), TRI))
              continue;
            int Cycle = It->second.second;
            G.LastCycle[I] = std::min(G.LastCycle[I], Cycle);
            if (!VU::VIRegClass.contains(MO.getReg()))
              continue;
            int Latency = getReadyLatency(MI, MO.getReg(), TRI);
            int Late = std::max(0, Cycle + Latency - CurrentCycles);
            G.LastFromEnd[I] = std::min(G.LastFromEnd[I], Late - Latency);
          }
        }

      // The resources of an iteration bound the number of words.
      const MCSchedModel &SM = STI.getSchedModel();
      // A field move that has a form in the other pipe counts for the pair
      // of resources of its two forms.
      std::vector<unsigned> Busy(SM.getNumProcResourceKinds(), 0);
      unsigned Movable = 0;
      SmallDenseSet<unsigned, 2> MovableRes;
      // Outside a loop, a unit that stays busy can be busy after the block.
      auto Count = [&](unsigned Cycles) { return IsLoop ? Cycles : 1u; };
      for (unsigned I = 0, E = G.Resources.size(); I != E; ++I) {
        if (G.AltOpc[I]) {
          ++Movable;
          for (auto [Idx, Cycles] : G.Resources[I])
            MovableRes.insert(Idx);
          for (auto [Idx, Cycles] : G.AltResources[I])
            MovableRes.insert(Idx);
          continue;
        }
        for (auto [Idx, Cycles] : G.Resources[I])
          Busy[Idx] += Count(Cycles);
      }
      int MinL = 2;
      unsigned PairBusy = Movable;
      for (unsigned Idx = 1, E = Busy.size(); Idx != E; ++Idx) {
        MinL = std::max<int>(
            MinL, divideCeil(Busy[Idx], SM.getProcResource(Idx)->NumUnits));
        if (MovableRes.contains(Idx))
          PairBusy += Busy[Idx];
      }
      if (Movable)
        MinL = std::max<int>(MinL, divideCeil(PairBusy, MovableRes.size()));
      if (!IsLoop)
        MinL = std::max(MinL, G.getMinLength());

      LLVM_DEBUG(dbgs() << printMBBReference(MBB) << ": " << MaxCycles
                        << " cycles, at least " << MinL << " words\n");
      // Each step narrows the windows of all instructions, so the search of
      // a large loop, such as an unrolled kernel, takes fewer of them.
      uint64_t N = G.Instrs.size();
      unsigned Steps = std::clamp<uint64_t>(200000000 / (N * N), 1000, 1000000);
      SmallVector<int, 32> Cycles;
      SmallVector<bool, 32> UseAlt;
      // The layout of a schedule in L words, if one is found in Budget steps.
      // It is empty, too, if it takes MaxCycles or more.
      auto TryLength = [&](int L, unsigned &Budget,
                           bool &Found) -> std::optional<Layout> {
        Found = G.schedule(L, Cycles, UseAlt, Budget);
        if (!Found) {
          LLVM_DEBUG(dbgs() << "  no schedule in " << L << " words\n");
          return std::nullopt;
        }
        bool HasBranch = G.Branch != LoopGraph::NoBranch;
        Layout New{SmallVector<VU::Word, 32>(L), 0,
                   G.Instrs[HasBranch ? G.Branch : 0]->getDebugLoc()};
        if (HasBranch)
          New.Words[L - 1].DelaySlot = true;
        // The words are counted with the field moves in their new form,
        // which changes back until the layout is taken.
        for (unsigned I = 0, E = G.Instrs.size(); I != E; ++I) {
          MachineInstr *MI = G.Instrs[I];
          if (UseAlt[I]) {
            switchForm(*MI, TII);
            New.Switched.push_back(MI);
          }
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
        if (IsLoop) {
          New.Cycles = VU::getLoopCycles(New.Words, TII, Itin, TRI);
        } else {
          DefMap Defs = Incoming;
          New.Cycles = getBlockCycles(New.Words, Defs, TII, Itin, TRI);
        }
        for (MachineInstr *MI : New.Switched)
          switchForm(*MI, TII);
        LLVM_DEBUG(dbgs() << printMBBReference(MBB) << ": " << MaxCycles
                          << " cycles, " << L << " words take " << New.Cycles
                          << "\n");
        if (New.Cycles >= MaxCycles)
          return std::nullopt;
        return New;
      };
      int L = MinL;
      for (; L < MaxCycles && Steps; ++L) {
        // A search for one length takes at most half of the steps that are
        // left, so a length that is hard to prove impossible, just below the
        // shortest schedule, leaves steps for the longer lengths.
        unsigned Budget = std::max(Steps / 2, std::min(Steps, 1000u));
        unsigned Left = Budget;
        bool Found;
        std::optional<Layout> New = TryLength(L, Left, Found);
        Steps -= Budget - Left;
        if (Found)
          return New;
      }
      // The steps ran out below the lengths that have a schedule, which can
      // be far below the current length: the lengths in between are bisected,
      // with a few steps each.
      std::optional<Layout> Best;
      for (int Lo = L, Hi = MaxCycles - 1; Lo <= Hi;) {
        int Mid = Lo + (Hi - Lo) / 2;
        unsigned Left = 1000;
        bool Found;
        std::optional<Layout> New = TryLength(Mid, Left, Found);
        if (New) {
          if (!Best || New->Cycles < Best->Cycles)
            Best = std::move(New);
          Hi = Mid - 1;
        } else {
          Lo = Mid + 1;
        }
      }
      return Best;
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
    for (MachineInstr *MI : Best->Switched)
      switchForm(*MI, TII);
    if (!IsLoop)
      removeEmptyWords(Best->Words, Best->Cycles, Incoming, TII, Itin, TRI);
    VU::rebuildWords(MBB, Best->Words, TII, Best->DL);
    for (MachineInstr *MI : Old)
      MI->eraseFromParent();
    // The order changed, and with it the last reads of the registers.
    recomputeLivenessFlags(MBB);
    Changed = true;
  }
  return Changed;
}
