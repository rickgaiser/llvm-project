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
// iteration.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUMCTargetDesc.h"
#include "VU.h"
#include "VUSubtarget.h"
#include "VUWords.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/MC/MCSchedule.h"
#include <functional>

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

  // Register dependences.
  DenseMap<Register, unsigned> LastDef;
  DenseMap<Register, SmallVector<unsigned, 4>> Readers, UpwardUses;
  SmallVector<unsigned, 8> MemOps;
  for (ArrayRef<unsigned> Word : Words) {
    for (unsigned N : Word)
      for (const MachineOperand &MO : Instrs[N]->all_uses()) {
        Register Reg = MO.getReg();
        if (!Reg || Reg == VU::VF0 || Reg == VU::VI0)
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
        for (unsigned M : MemOps) {
          const MachineInstr &Other = *Instrs[M];
          if ((MI.mayStore() || Other.mayStore()) &&
              !TII.areMemAccessesTriviallyDisjoint(Other, MI))
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
      if (propagate(Edges, L, Release, Deadline)) {
        Placed[Op] = true;
        Reserve(Op, C, 1);
        if (Search(NumPlaced + 1))
          return true;
        Reserve(Op, C, -1);
        Placed[Op] = false;
      }
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
  if (!Search(1)) {
    LLVM_DEBUG(dbgs() << "  no schedule found in " << Steps << " steps\n");
    return false;
  }
  LLVM_DEBUG(dbgs() << "  schedule in " << L << " words found in " << Steps
                    << " steps\n");
  Cycles.assign(Release.begin(), Release.end());
  return true;
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
    LoopGraph G(STI);
    if (!G.build(MBB)) {
      LLVM_DEBUG(dbgs() << printMBBReference(MBB) << ": not a simple loop\n");
      continue;
    }

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

    LLVM_DEBUG(dbgs() << printMBBReference(MBB) << ": " << CurrentCycles
                      << " cycles, at least " << MinL << " words\n");
    SmallVector<int, 32> Cycles;
    for (int L = MinL; L < CurrentCycles; ++L) {
      if (!G.schedule(L, Cycles)) {
        LLVM_DEBUG(dbgs() << "  no schedule in " << L << " words\n");
        continue;
      }
      SmallVector<VU::Word, 32> Words(L);
      Words[L - 1].DelaySlot = true;
      for (unsigned I = 0, E = G.Instrs.size(); I != E; ++I) {
        MachineInstr *MI = G.Instrs[I];
        (VU::isLowerInstr(MI->getDesc()) ? Words[Cycles[I]].Lower
                                         : Words[Cycles[I]].Upper) = MI;
      }
      int NewCycles = VU::getLoopCycles(Words, TII, Itin, TRI);
      LLVM_DEBUG(dbgs() << printMBBReference(MBB) << ": " << CurrentCycles
                        << " cycles, " << L << " words take " << NewCycles
                        << "\n");
      if (NewCycles >= CurrentCycles)
        break;
      // Remove the NOPs and delay slot markers, which are rebuilt.
      SmallVector<MachineInstr *, 4> Old;
      for (MachineInstr &MI : MBB.instrs())
        if (MI.getOpcode() == VU::NOP || MI.getOpcode() == VU::DSNOP ||
            MI.getOpcode() == VU::DELAY_SLOT)
          Old.push_back(&MI);
      VU::rebuildWords(MBB, Words, TII, G.Instrs[G.Branch]->getDebugLoc());
      for (MachineInstr *MI : Old)
        MI->eraseFromParent();
      Changed = true;
      break;
    }
  }
  return Changed;
}
