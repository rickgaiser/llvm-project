//===-- MipsR5900PipelineBalancer.cpp - R5900 Dual Pipeline Optimizer -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass balances multiply/divide operations between Pipeline 0 (MAC0)
// and Pipeline 1 (MAC1) to maximize instruction-level parallelism on R5900.
//
// R5900 has two independent multiply/divide pipelines:
// - Pipeline 0 (MAC0): Uses HI0/LO0, instructions: MULT, MADD, DIV
// - Pipeline 1 (MAC1): Uses HI1/LO1, instructions: MULT1, MADD1, DIV1
//
// This pass runs after register allocation when physical register usage is
// known, allowing precise liveness analysis of HI/LO registers.
//
//===----------------------------------------------------------------------===//

#include "Mips.h"
#include "MipsInstrInfo.h"
#include "MipsSubtarget.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "r5900-pipeline-balancer"

namespace {

/// Tracks liveness state of HI/LO registers for both pipelines
struct PipelineState {
  bool HI0Live = false;  // HI0 contains pending result
  bool LO0Live = false;  // LO0 contains pending result
  bool HI1Live = false;  // HI1 contains pending result
  bool LO1Live = false;  // LO1 contains pending result

  bool operator==(const PipelineState &Other) const {
    return HI0Live == Other.HI0Live && LO0Live == Other.LO0Live &&
           HI1Live == Other.HI1Live && LO1Live == Other.LO1Live;
  }

  bool operator!=(const PipelineState &Other) const {
    return !(*this == Other);
  }

  PipelineState operator|(const PipelineState &Other) const {
    PipelineState Result;
    Result.HI0Live = HI0Live || Other.HI0Live;
    Result.LO0Live = LO0Live || Other.LO0Live;
    Result.HI1Live = HI1Live || Other.HI1Live;
    Result.LO1Live = LO1Live || Other.LO1Live;
    return Result;
  }

  PipelineState &operator|=(const PipelineState &Other) {
    *this = *this | Other;
    return *this;
  }

  void clear() {
    HI0Live = LO0Live = HI1Live = LO1Live = false;
  }
};

class MipsR5900PipelineBalancer : public MachineFunctionPass {
public:
  static char ID;
  MipsR5900PipelineBalancer() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override {
    return "R5900 Pipeline Balancer";
  }

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::NoVRegs);
  }

private:
  const MipsSubtarget *STI = nullptr;
  const MipsInstrInfo *TII = nullptr;

  // Per-basic-block liveness info
  DenseMap<MachineBasicBlock *, PipelineState> LiveIn;
  DenseMap<MachineBasicBlock *, PipelineState> LiveOut;

  void computeHILOLiveness(MachineFunction &MF);
  bool balanceBasicBlock(MachineBasicBlock &MBB);
  bool isPipeline0MultDiv(unsigned Opcode) const;
  unsigned getPipeline1Opcode(unsigned Opcode) const;
  void updateLivenessForInstr(const MachineInstr &MI, PipelineState &State,
                              bool Forward) const;
  bool definesHI0LO0(const MachineInstr &MI) const;
  bool usesHI0LO0(const MachineInstr &MI) const;
  bool definesHI1LO1(const MachineInstr &MI) const;
  bool usesHI1LO1(const MachineInstr &MI) const;
  bool willHI0LO0BeUsedAfter(MachineBasicBlock::iterator It,
                              MachineBasicBlock &MBB) const;
  bool willHI1LO1BeUsedAfter(MachineBasicBlock::iterator It,
                              MachineBasicBlock &MBB) const;
};

} // end anonymous namespace

char MipsR5900PipelineBalancer::ID = 0;

INITIALIZE_PASS(MipsR5900PipelineBalancer, DEBUG_TYPE,
                "R5900 Pipeline Balancer", false, false)

/// Check if this is a Pipeline 0 multiply/divide operation
bool MipsR5900PipelineBalancer::isPipeline0MultDiv(unsigned Opcode) const {
  switch (Opcode) {
  case Mips::R5900_MULT:
  case Mips::R5900_MULTU:
  case Mips::R5900_MADD:
  case Mips::R5900_MADDU:
  case Mips::PseudoSDIV:
  case Mips::PseudoUDIV:
  case Mips::SDIV:
  case Mips::UDIV:
    return true;
  default:
    return false;
  }
}

/// Get the Pipeline 1 equivalent of a Pipeline 0 opcode
unsigned MipsR5900PipelineBalancer::getPipeline1Opcode(unsigned Opcode) const {
  switch (Opcode) {
  case Mips::R5900_MULT:
    return Mips::MULT1;
  case Mips::R5900_MULTU:
    return Mips::MULTU1;
  case Mips::R5900_MADD:
    return Mips::MADD1;
  case Mips::R5900_MADDU:
    return Mips::MADDU1;
  case Mips::PseudoSDIV:
  case Mips::SDIV:
    return Mips::DIV1;
  case Mips::PseudoUDIV:
  case Mips::UDIV:
    return Mips::DIVU1;
  default:
    return 0;
  }
}

/// Check if instruction defines HI0 or LO0
bool MipsR5900PipelineBalancer::definesHI0LO0(const MachineInstr &MI) const {
  for (const MachineOperand &MO : MI.operands()) {
    if (MO.isReg() && MO.isDef()) {
      Register Reg = MO.getReg();
      if (Reg == Mips::HI0 || Reg == Mips::LO0 ||
          Reg == Mips::HI0_64 || Reg == Mips::LO0_64)
        return true;
    }
  }
  // Also check implicit defs
  for (MCPhysReg ImpDef : MI.getDesc().implicit_defs()) {
    if (ImpDef == Mips::HI0 || ImpDef == Mips::LO0 ||
        ImpDef == Mips::HI0_64 || ImpDef == Mips::LO0_64)
      return true;
  }
  return false;
}

/// Check if instruction uses HI0 or LO0
bool MipsR5900PipelineBalancer::usesHI0LO0(const MachineInstr &MI) const {
  for (const MachineOperand &MO : MI.operands()) {
    if (MO.isReg() && MO.isUse()) {
      Register Reg = MO.getReg();
      if (Reg == Mips::HI0 || Reg == Mips::LO0 ||
          Reg == Mips::HI0_64 || Reg == Mips::LO0_64)
        return true;
    }
  }
  // Also check implicit uses
  for (MCPhysReg ImpUse : MI.getDesc().implicit_uses()) {
    if (ImpUse == Mips::HI0 || ImpUse == Mips::LO0 ||
        ImpUse == Mips::HI0_64 || ImpUse == Mips::LO0_64)
      return true;
  }
  return false;
}

/// Check if instruction defines HI1 or LO1
bool MipsR5900PipelineBalancer::definesHI1LO1(const MachineInstr &MI) const {
  for (const MachineOperand &MO : MI.operands()) {
    if (MO.isReg() && MO.isDef()) {
      Register Reg = MO.getReg();
      if (Reg == Mips::HI1 || Reg == Mips::LO1)
        return true;
    }
  }
  for (MCPhysReg ImpDef : MI.getDesc().implicit_defs()) {
    if (ImpDef == Mips::HI1 || ImpDef == Mips::LO1)
      return true;
  }
  return false;
}

/// Check if instruction uses HI1 or LO1
bool MipsR5900PipelineBalancer::usesHI1LO1(const MachineInstr &MI) const {
  for (const MachineOperand &MO : MI.operands()) {
    if (MO.isReg() && MO.isUse()) {
      Register Reg = MO.getReg();
      if (Reg == Mips::HI1 || Reg == Mips::LO1)
        return true;
    }
  }
  for (MCPhysReg ImpUse : MI.getDesc().implicit_uses()) {
    if (ImpUse == Mips::HI1 || ImpUse == Mips::LO1)
      return true;
  }
  return false;
}

/// Check if any instruction after It in MBB will use HI0/LO0
/// This is used to prevent converting a MULT to MULT1 if a subsequent
/// MADD instruction needs the result in HI0/LO0.
bool MipsR5900PipelineBalancer::willHI0LO0BeUsedAfter(
    MachineBasicBlock::iterator It, MachineBasicBlock &MBB) const {
  // Start from the instruction after It
  auto NextIt = std::next(It);
  for (auto I = NextIt; I != MBB.end(); ++I) {
    // Check if this instruction uses HI0/LO0
    if (usesHI0LO0(*I))
      return true;
    // If this instruction defines HI0/LO0 without using it first,
    // then subsequent uses would read the new value, not ours
    if (definesHI0LO0(*I))
      return false;
  }
  // Also check LiveOut - if HI0/LO0 is live out, it will be used
  auto It2 = LiveOut.find(&MBB);
  if (It2 != LiveOut.end())
    return It2->second.HI0Live || It2->second.LO0Live;
  return false;
}

/// Check if any instruction after It in MBB will use HI1/LO1
/// This is used to determine if we need to keep HI1/LO1 live after
/// a MULT1 conversion. For R5900's 3-operand form, HI1/LO1 are only
/// needed if a subsequent MFHI1/MFLO1 will read them.
bool MipsR5900PipelineBalancer::willHI1LO1BeUsedAfter(
    MachineBasicBlock::iterator It, MachineBasicBlock &MBB) const {
  // Start from the instruction after It
  auto NextIt = std::next(It);
  for (auto I = NextIt; I != MBB.end(); ++I) {
    // Check if this instruction uses HI1/LO1
    if (usesHI1LO1(*I))
      return true;
    // If this instruction defines HI1/LO1 without using it first,
    // then subsequent uses would read the new value, not ours
    if (definesHI1LO1(*I))
      return false;
  }
  // Also check LiveOut - if HI1/LO1 is live out, it will be used
  auto It2 = LiveOut.find(&MBB);
  if (It2 != LiveOut.end())
    return It2->second.HI1Live || It2->second.LO1Live;
  return false;
}

/// Update liveness state based on instruction
/// Forward=true: update after executing instruction
/// Forward=false: update before executing instruction (for backward analysis)
void MipsR5900PipelineBalancer::updateLivenessForInstr(
    const MachineInstr &MI, PipelineState &State, bool Forward) const {
  if (Forward) {
    // Forward pass: defs kill liveness, uses create liveness
    if (definesHI0LO0(MI)) {
      State.HI0Live = true;
      State.LO0Live = true;
    }
    if (definesHI1LO1(MI)) {
      State.HI1Live = true;
      State.LO1Live = true;
    }
    // Check for MFHI/MFLO which consume the result
    unsigned Opc = MI.getOpcode();
    if (Opc == Mips::MFHI || Opc == Mips::MFHI64) {
      State.HI0Live = false;
    }
    if (Opc == Mips::MFLO || Opc == Mips::MFLO64) {
      State.LO0Live = false;
    }
    if (Opc == Mips::MFHI1) {
      State.HI1Live = false;
    }
    if (Opc == Mips::MFLO1) {
      State.LO1Live = false;
    }
  } else {
    // Backward pass: uses make live, defs kill
    if (usesHI0LO0(MI)) {
      State.HI0Live = true;
      State.LO0Live = true;
    }
    if (usesHI1LO1(MI)) {
      State.HI1Live = true;
      State.LO1Live = true;
    }
    if (definesHI0LO0(MI)) {
      State.HI0Live = false;
      State.LO0Live = false;
    }
    if (definesHI1LO1(MI)) {
      State.HI1Live = false;
      State.LO1Live = false;
    }
  }
}

/// Compute HI/LO liveness across the entire function using backward dataflow
void MipsR5900PipelineBalancer::computeHILOLiveness(MachineFunction &MF) {
  // Initialize
  LiveIn.clear();
  LiveOut.clear();
  for (MachineBasicBlock &MBB : MF) {
    LiveIn[&MBB] = PipelineState();
    LiveOut[&MBB] = PipelineState();
  }

  // Backward dataflow analysis
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (MachineBasicBlock &MBB : reverse(MF)) {
      PipelineState OldLiveIn = LiveIn[&MBB];

      // LiveOut = union of successor LiveIns
      PipelineState NewLiveOut;
      for (MachineBasicBlock *Succ : MBB.successors()) {
        NewLiveOut |= LiveIn[Succ];
      }
      LiveOut[&MBB] = NewLiveOut;

      // Compute LiveIn by walking backward through BB
      PipelineState State = NewLiveOut;
      for (MachineInstr &MI : reverse(MBB)) {
        updateLivenessForInstr(MI, State, /*Forward=*/false);
      }

      if (State != OldLiveIn) {
        LiveIn[&MBB] = State;
        Changed = true;
      }
    }
  }

  LLVM_DEBUG({
    dbgs() << "HI/LO Liveness Analysis:\n";
    for (MachineBasicBlock &MBB : MF) {
      dbgs() << "  BB#" << MBB.getNumber() << ": LiveIn=["
             << (LiveIn[&MBB].HI0Live ? "HI0," : "")
             << (LiveIn[&MBB].LO0Live ? "LO0," : "")
             << (LiveIn[&MBB].HI1Live ? "HI1," : "")
             << (LiveIn[&MBB].LO1Live ? "LO1" : "") << "] LiveOut=["
             << (LiveOut[&MBB].HI0Live ? "HI0," : "")
             << (LiveOut[&MBB].LO0Live ? "LO0," : "")
             << (LiveOut[&MBB].HI1Live ? "HI1," : "")
             << (LiveOut[&MBB].LO1Live ? "LO1" : "") << "]\n";
    }
  });
}

/// Balance pipelines in a single basic block
bool MipsR5900PipelineBalancer::balanceBasicBlock(MachineBasicBlock &MBB) {
  bool Changed = false;
  PipelineState State = LiveIn[&MBB];

  // Collect instructions to convert first, then convert them
  // This avoids iterator invalidation issues and allows proper alternating
  SmallVector<MachineInstr *, 8> ToConvert;

  // Check if this block has a MADD chain (affects conversion strategy)
  bool HasMADDChain = false;
  for (const MachineInstr &MI : MBB) {
    unsigned Opc = MI.getOpcode();
    if (Opc == Mips::R5900_MADD || Opc == Mips::R5900_MADDU) {
      HasMADDChain = true;
      break;
    }
  }

  for (auto It = MBB.begin(); It != MBB.end(); ++It) {
    MachineInstr &MI = *It;
    unsigned Opc = MI.getOpcode();

    // Check if this is a Pipeline 0 multiply/divide that could be converted
    if (isPipeline0MultDiv(Opc)) {
      unsigned P1Opc = getPipeline1Opcode(Opc);

      // Only convert if:
      // 1. Pipeline 1 equivalent exists
      // 2. HI1/LO1 are available (not live, or live but won't be used)
      // 3. This is not a MADD (MADD reads HI0/LO0)
      // 4. HI0/LO0 won't be used by subsequent instructions
      //    (e.g., a following MADD needs the result in HI0/LO0)
      // 5. Either P0 was just used (alternating) or there's a MADD chain
      //
      // Strategy depends on whether the block has a MADD chain:
      // - With MADD chain: convert independent multiplies to P1 (parallelism)
      // - Without MADD chain: alternate P0/P1 for parallelism
      //
      // For R5900's 3-operand form, HI/LO are written but often dead
      // (result goes to GPR, not via MFHI/MFLO). We can convert even
      // if HI1/LO1 are "live" from a previous MULT1, as long as those
      // values won't be read (no MFHI1/MFLO1 will use them).
      bool IsMADD = (Opc == Mips::R5900_MADD || Opc == Mips::R5900_MADDU);
      bool HI0LO0NeededLater = willHI0LO0BeUsedAfter(It, MBB);
      bool HI1LO1Available = (!State.HI1Live && !State.LO1Live) ||
                              !willHI1LO1BeUsedAfter(It, MBB);

      // For alternating: convert to P1 if P0 was just used
      bool Pipeline0JustUsed = (State.HI0Live || State.LO0Live);

      // Decision: always alternate for maximum parallelism.
      // The HI0LO0NeededLater check protects MADD chain multiplies.
      // HasMADDChain is used to ensure we don't skip the first independent
      // multiply when there's a MADD chain that will use P0 anyway.
      bool ShouldConvert = Pipeline0JustUsed ||
                           (HasMADDChain && !HI0LO0NeededLater);

      if (P1Opc && HI1LO1Available && !IsMADD && !HI0LO0NeededLater &&
          ShouldConvert) {
        LLVM_DEBUG(dbgs() << "Will convert to Pipeline 1: " << MI);
        ToConvert.push_back(&MI);

        // Update state - mark P1 as used for alternating
        // Only truly "live" if HI1/LO1 will be read later
        if (willHI1LO1BeUsedAfter(It, MBB)) {
          State.HI1Live = true;
          State.LO1Live = true;
        }
        // Clear P0 state since we're alternating
        State.HI0Live = false;
        State.LO0Live = false;
      } else {
        // Using Pipeline 0
        State.HI0Live = true;
        State.LO0Live = true;
        // Clear P1 state for alternating
        if (!willHI1LO1BeUsedAfter(It, MBB)) {
          State.HI1Live = false;
          State.LO1Live = false;
        }
      }
    }

    // Update liveness state for non-multiply instructions
    if (!isPipeline0MultDiv(MI.getOpcode())) {
      updateLivenessForInstr(MI, State, /*Forward=*/true);
    }
  }

  // Now perform the conversions
  for (MachineInstr *MI : ToConvert) {
    unsigned P1Opc = getPipeline1Opcode(MI->getOpcode());
    LLVM_DEBUG(dbgs() << "Converting to Pipeline 1: " << *MI);

    // Create the Pipeline 1 instruction
    // Only copy explicit operands - let the instruction definition add
    // the correct implicit-defs ($hi1/$lo1 instead of $hi0/$lo0)
    MachineInstrBuilder MIB =
        BuildMI(MBB, *MI, MI->getDebugLoc(), TII->get(P1Opc));

    // Copy only explicit operands (GPR result and two source regs)
    for (const MachineOperand &MO : MI->explicit_operands()) {
      MIB.add(MO);
    }

    // Remove the old instruction
    MI->eraseFromParent();
    Changed = true;
  }

  return Changed;
}

bool MipsR5900PipelineBalancer::runOnMachineFunction(MachineFunction &MF) {
  STI = &MF.getSubtarget<MipsSubtarget>();

  // Only run on R5900
  if (!STI->isR5900())
    return false;

  TII = STI->getInstrInfo();

  LLVM_DEBUG(dbgs() << "*** R5900 Pipeline Balancer: " << MF.getName()
                    << " ***\n");

  // Step 1: Compute HI/LO liveness across all basic blocks
  computeHILOLiveness(MF);

  // Step 2: Balance pipelines with full liveness info
  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    Changed |= balanceBasicBlock(MBB);
  }

  return Changed;
}

FunctionPass *llvm::createMipsR5900PipelineBalancerPass() {
  return new MipsR5900PipelineBalancer();
}
