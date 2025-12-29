//===-- DVPVUVLIWPacketizer.cpp - DVPVU VLIW Packetizer -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the DVPVU VLIW Packetizer pass.
//
// The VU executes 64-bit instruction words containing:
// - Upper (bits 63-32): FMAC operations
// - Lower (bits 31-0): Various operations (IALU, LSU, BRU, etc.)
//
// This pass bundles compatible Upper+Lower instruction pairs together.
// Each instruction is already encoded with NOP in the unused slot, so
// bundling just means marking them as a bundle so the AsmPrinter can
// combine their encodings.
//
//===----------------------------------------------------------------------===//

#include "DVPVUVLIWPacketizer.h"
#include "DVPVU.h"
#include "DVPVUInstrInfo.h"
#include "DVPVUSubtarget.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBundle.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "dvpvu-vliw-packetizer"

// TSFlags bit positions (must match DVPVUInstrFormats.td)
namespace {
enum DVPVUTSFlags {
  TSF_IsUpper = 0x1,  // Bit 0: Uses Upper slot
  TSF_IsLower = 0x2   // Bit 1: Uses Lower slot
};
} // anonymous namespace

char DVPVUVLIWPacketizer::ID = 0;

INITIALIZE_PASS(DVPVUVLIWPacketizer, DEBUG_TYPE, "DVPVU VLIW Packetizer",
                false, false)

DVPVUVLIWPacketizer::DVPVUVLIWPacketizer() : MachineFunctionPass(ID) {
  initializeDVPVUVLIWPacketizerPass(*PassRegistry::getPassRegistry());
}

void DVPVUVLIWPacketizer::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  MachineFunctionPass::getAnalysisUsage(AU);
}

bool DVPVUVLIWPacketizer::isUpperSlot(const MachineInstr &MI) const {
  if (MI.isDebugInstr() || MI.isLabel())
    return false;

  uint64_t TSFlags = MI.getDesc().TSFlags;
  return (TSFlags & TSF_IsUpper) != 0;
}

bool DVPVUVLIWPacketizer::isLowerSlot(const MachineInstr &MI) const {
  if (MI.isDebugInstr() || MI.isLabel())
    return false;

  uint64_t TSFlags = MI.getDesc().TSFlags;
  return (TSFlags & TSF_IsLower) != 0;
}

bool DVPVUVLIWPacketizer::hasDataHazard(const MachineInstr &First,
                                         const MachineInstr &Second) const {
  // In VLIW parallel execution, Upper and Lower execute simultaneously:
  // - Both read inputs at cycle start
  // - Both write outputs at cycle end
  //
  // This means RAW (Read After Write) is NOT a hazard - the read sees
  // the old value, which is correct for parallel semantics.
  //
  // WAW (Write After Write) IS a hazard - both writing the same register
  // would give undefined results.
  //
  // WAR (Write After Read) is NOT a hazard - the read sees the old value.

  // Check for WAW (Write After Write) hazards only
  for (const MachineOperand &Def1 : First.defs()) {
    if (!Def1.isReg())
      continue;
    Register Def1Reg = Def1.getReg();
    if (!Def1Reg.isValid())
      continue;

    for (const MachineOperand &Def2 : Second.defs()) {
      if (!Def2.isReg())
        continue;
      if (Def2.getReg() == Def1Reg)
        return true;
    }
  }

  return false;
}

bool DVPVUVLIWPacketizer::canBundle(const MachineInstr &Upper,
                                     const MachineInstr &Lower) const {
  // Both instructions must be valid for their slots
  if (!isUpperSlot(Upper) || !isLowerSlot(Lower))
    return false;

  // Check for data hazards - in the VU, Upper and Lower execute in parallel,
  // so they cannot have dependencies on each other
  if (hasDataHazard(Upper, Lower) || hasDataHazard(Lower, Upper))
    return false;

  return true;
}

bool DVPVUVLIWPacketizer::packetizeMBB(MachineBasicBlock &MBB) {
  bool Changed = false;

  // Simple greedy algorithm:
  // - Scan instructions in order
  // - When we find an Upper instruction followed by a Lower (or vice versa),
  //   try to bundle them
  // - For now, we don't reorder instructions

  for (MachineBasicBlock::iterator I = MBB.begin(), E = MBB.end(); I != E;) {
    MachineInstr &MI = *I;

    // Skip debug and label instructions
    if (MI.isDebugInstr() || MI.isLabel() || MI.isBundle()) {
      ++I;
      continue;
    }

    // Check if this instruction has a slot assignment
    bool IsUpper = isUpperSlot(MI);
    bool IsLower = isLowerSlot(MI);

    // If no slot assignment (pseudo instruction), skip
    if (!IsUpper && !IsLower) {
      ++I;
      continue;
    }

    // Look at the next instruction
    MachineBasicBlock::iterator NextI = std::next(I);
    if (NextI == E) {
      ++I;
      continue;
    }

    // Skip debug instructions and labels to find the real next instruction
    while (NextI != E && (NextI->isDebugInstr() || NextI->isLabel())) {
      ++NextI;
    }

    if (NextI == E) {
      ++I;
      continue;
    }

    MachineInstr &RealNextMI = *NextI;
    bool NextIsUpper = isUpperSlot(RealNextMI);
    bool NextIsLower = isLowerSlot(RealNextMI);

    // Try to bundle Upper+Lower pair
    bool CanBundle = false;
    MachineInstr *UpperMI = nullptr;
    MachineInstr *LowerMI = nullptr;

    if (IsUpper && NextIsLower) {
      UpperMI = &MI;
      LowerMI = &RealNextMI;
      CanBundle = canBundle(*UpperMI, *LowerMI);
    } else if (IsLower && NextIsUpper) {
      LowerMI = &MI;
      UpperMI = &RealNextMI;
      CanBundle = canBundle(*UpperMI, *LowerMI);
    }

    if (CanBundle) {
      LLVM_DEBUG(dbgs() << "Bundling:\n  Upper: " << *UpperMI
                        << "  Lower: " << *LowerMI);

      // Create a bundle with Upper first, then Lower
      // LLVM bundles use BUNDLE pseudo instruction as header
      MachineBasicBlock::instr_iterator BundleStart = I.getInstrIterator();
      MachineBasicBlock::instr_iterator BundleEnd =
          std::next(NextI).getInstrIterator();

      // Use LLVM's bundling utilities
      // The bundle will contain both instructions
      finalizeBundle(MBB, BundleStart, BundleEnd);

      Changed = true;

      // Move past the bundle (now it's a single bundle instruction)
      I = std::next(I);
    } else {
      // No bundling possible, move to next instruction
      ++I;
    }
  }

  return Changed;
}

bool DVPVUVLIWPacketizer::runOnMachineFunction(MachineFunction &MF) {
  LLVM_DEBUG(dbgs() << "*** DVPVU VLIW Packetizer: " << MF.getName() << " ***\n");

  const DVPVUSubtarget &ST = MF.getSubtarget<DVPVUSubtarget>();
  TII = ST.getInstrInfo();

  bool Changed = false;

  for (MachineBasicBlock &MBB : MF)
    Changed |= packetizeMBB(MBB);

  return Changed;
}

FunctionPass *llvm::createDVPVUVLIWPacketizerPass() {
  return new DVPVUVLIWPacketizer();
}
