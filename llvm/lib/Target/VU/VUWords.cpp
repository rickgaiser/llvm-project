//===-- VUWords.cpp - Micro instruction words of a VU loop ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "VUWords.h"
#include "MCTargetDesc/VUMCTargetDesc.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineInstrBundle.h"
#include "llvm/CodeGen/TargetInstrInfo.h"

using namespace llvm;
using namespace llvm::VU;

SmallVector<MachineInstr *, 2> VU::getWordInstrs(MachineInstr &W) {
  if (!W.isBundle())
    return {&W};
  SmallVector<MachineInstr *, 2> Instrs;
  for (auto I = std::next(W.getIterator());
       I != W.getParent()->instr_end() && I->isInsideBundle(); ++I)
    if (!I->isMetaInstruction())
      Instrs.push_back(&*I);
  return Instrs;
}

int VU::getLoopCycles(ArrayRef<Word> Words, const TargetInstrInfo &TII,
                      const InstrItineraryData *Itin,
                      const TargetRegisterInfo &TRI) {
  DenseMap<Register, std::pair<MachineInstr *, int>> LastDef;
  int Cycle = -1, Start = 0;
  for (int Iter = 0; Iter != 3; ++Iter) {
    // The second iteration starts after all the latencies of the first.
    if (Iter == 2)
      return Cycle + 1 - Start;
    Start = Cycle + 1;
    for (const Word &W : Words) {
      int Issue = Cycle + 1;
      for (MachineInstr *MI : {W.Upper, W.Lower}) {
        if (!MI)
          continue;
        // An early read of Q or P gets a WAITQ or WAITP (VUHazards), so it
        // waits like a read of any other register.
        for (const MachineOperand &MO : MI->all_uses()) {
          Register Reg = MO.getReg();
          auto It = LastDef.find(Reg);
          if (It == LastDef.end())
            continue;
          auto [Def, DefCycle] = It->second;
          int DefIdx = Def->findRegisterDefOperandIdx(Reg, &TRI);
          if (DefIdx < 0)
            continue;
          int Latency =
              TII.getOperandLatency(Itin, *Def, DefIdx, *MI, MO.getOperandNo())
                  .value_or(1);
          Issue = std::max(Issue, DefCycle + Latency);
        }
      }
      Cycle = Issue;
      for (MachineInstr *MI : {W.Upper, W.Lower})
        if (MI)
          for (const MachineOperand &MO : MI->all_defs())
            LastDef[MO.getReg()] = {MI, Cycle};
    }
  }
  llvm_unreachable("returns in the third iteration");
}

void VU::rebuildWords(MachineBasicBlock &MBB, ArrayRef<Word> Words,
                      const TargetInstrInfo &TII, const DebugLoc &DL) {
  for (MachineInstr &W : make_early_inc_range(MBB)) {
    if (!W.isBundle())
      continue;
    for (MachineInstr *MI = W.getNextNode(); MI && MI->isBundledWithPred();) {
      MachineInstr *Next = MI->getNextNode();
      MI->unbundleFromPred();
      MI = Next;
    }
    W.eraseFromParent();
  }
  for (const Word &W : Words) {
    SmallVector<MachineInstr *, 3> Instrs;
    for (MachineInstr *MI : {W.Upper, W.Lower})
      if (MI) {
        MBB.splice(MBB.end(), &MBB, MI);
        Instrs.push_back(MI);
      }
    if (Instrs.empty()) {
      BuildMI(&MBB, DL, TII.get(W.DelaySlot ? VU::DSNOP : VU::NOP));
      continue;
    }
    if (W.DelaySlot)
      Instrs.push_back(BuildMI(&MBB, DL, TII.get(VU::DELAY_SLOT)));
    if (Instrs.size() > 1)
      finalizeBundle(MBB, MachineBasicBlock::instr_iterator(Instrs.front()),
                     MBB.instr_end());
  }
}
