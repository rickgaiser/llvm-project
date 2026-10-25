//===-- VULoopLayout.cpp - Single block loops fall into their exits -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The delay slot filler and the loop scheduler work on single block loops
// that end in their only branch, with the exit after them. In a vertex
// program the batches are a loop around the item loop (from one YIELD to the
// next), and block placement may rotate it so that the end of a batch falls
// into the start of the next, and the item loop jumps to its exit instead.
// This pass moves the exit, with the blocks that it falls into up to the end
// of the batch (the block with the YIELD), back behind the item loop: the end
// of the batch then jumps to its start, once per batch, and the item loop
// keeps the form those passes take.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUMCTargetDesc.h"
#include "VU.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

using namespace llvm;

#define DEBUG_TYPE "vu-loop-layout"

namespace {
class VULoopLayout : public MachineFunctionPass {
public:
  static char ID;
  VULoopLayout() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "VU Loop Layout"; }
  bool runOnMachineFunction(MachineFunction &MF) override;
};
} // namespace

char VULoopLayout::ID = 0;

INITIALIZE_PASS(VULoopLayout, DEBUG_TYPE, "VU Loop Layout", false, false)

FunctionPass *llvm::createVULoopLayoutPass() { return new VULoopLayout(); }

static bool hasYield(const MachineBasicBlock &MBB) {
  return any_of(
      MBB, [](const MachineInstr &MI) { return MI.getOpcode() == VU::YIELD; });
}

bool VULoopLayout::runOnMachineFunction(MachineFunction &MF) {
  if (none_of(MF, hasYield))
    return false;
  bool Changed = false;
  SmallVector<MachineBasicBlock *, 16> Blocks;
  for (MachineBasicBlock &MBB : MF)
    Blocks.push_back(&MBB);
  for (MachineBasicBlock *Loop : Blocks) {
    if (Loop->succ_size() != 2 || !Loop->isSuccessor(Loop))
      continue;
    MachineBasicBlock *Exit = *Loop->succ_begin() == Loop
                                  ? *std::next(Loop->succ_begin())
                                  : *Loop->succ_begin();
    if (Loop->isLayoutSuccessor(Exit) || Exit->isEntryBlock())
      continue;
    // Nothing may fall into the exit.
    MachineBasicBlock *Prev = Exit->getPrevNode();
    if (Prev && Prev->canFallThrough())
      continue;
    // The exit and what it falls into, up to the end of the batch.
    SmallVector<MachineBasicBlock *, 8> Chain;
    bool Cut = false;
    for (MachineBasicBlock *B = Exit; B && B != Loop; B = B->getNextNode()) {
      Chain.push_back(B);
      if (!B->canFallThrough() || hasYield(*B)) {
        Cut = true;
        break;
      }
    }
    if (!Cut)
      continue;
    MachineBasicBlock *Last = Chain.back();
    MachineBasicBlock *LastNext = Last->getNextNode();
    MachineBasicBlock *LoopNext = Loop->getNextNode();
    MachineBasicBlock *After = Loop;
    for (MachineBasicBlock *B : Chain) {
      B->moveAfter(After);
      After = B;
    }
    Loop->updateTerminator(LoopNext);
    Last->updateTerminator(LastNext);
    if (Prev)
      Prev->updateTerminator(Exit);
    LLVM_DEBUG(dbgs() << printMBBReference(*Loop) << ": exit "
                      << printMBBReference(*Exit) << " moved behind it\n");
    Changed = true;
  }
  return Changed;
}
