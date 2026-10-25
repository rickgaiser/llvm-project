//===-- VUInstrInfo.h - VU instruction information --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_VU_VUINSTRINFO_H
#define LLVM_LIB_TARGET_VU_VUINSTRINFO_H

#include "VURegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"

#define GET_INSTRINFO_HEADER
#include "VUGenInstrInfo.inc"

namespace llvm {

class VUSubtarget;

class VUInstrInfo : public VUGenInstrInfo {
  const VURegisterInfo RI;

public:
  explicit VUInstrInfo(const VUSubtarget &STI);

  const VURegisterInfo &getRegisterInfo() const { return RI; }

  void copyPhysReg(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
                   const DebugLoc &DL, Register DestReg, Register SrcReg,
                   bool KillSrc, bool RenamableDest = false,
                   bool RenamableSrc = false) const override;

  void storeRegToStackSlot(
      MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI, Register SrcReg,
      bool isKill, int FrameIndex, const TargetRegisterClass *RC, Register VReg,
      MachineInstr::MIFlag Flags = MachineInstr::NoFlags) const override;

  void loadRegFromStackSlot(
      MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
      Register DestReg, int FrameIdx, const TargetRegisterClass *RC,
      Register VReg, unsigned SubReg = 0,
      MachineInstr::MIFlag Flags = MachineInstr::NoFlags) const override;

  bool expandPostRAPseudo(MachineInstr &MI) const override;

  bool isReMaterializableImpl(const MachineInstr &MI) const override;

  /// Nothing is hoisted out of a loop over program runs (one with a YIELD,
  /// the batches of a vertex program): it would take a register in every
  /// batch to save an instruction in the init.
  bool shouldHoist(const MachineInstr &MI,
                   const MachineLoop *FromLoop) const override;

  // Scheduling. All latencies are in micro instruction words.

  /// Returns the number of words after \p MI until its result in operand
  /// \p DefIdx can be read without a stall (VUSchedule.td).
  static unsigned getResultLatency(const MachineInstr &MI, unsigned DefIdx);
  DFAPacketizer *
  CreateTargetScheduleState(const TargetSubtargetInfo &STI) const override;
  std::optional<unsigned> getOperandLatency(const InstrItineraryData *ItinData,
                                            const MachineInstr &DefMI,
                                            unsigned DefIdx,
                                            const MachineInstr &UseMI,
                                            unsigned UseIdx) const override;

  // Memory accesses, for alias analysis and the pipeliner. Addresses are in
  // quadwords; offsets and increments are reported in bytes.
  bool getMemOperandsWithOffsetWidth(
      const MachineInstr &MI, SmallVectorImpl<const MachineOperand *> &BaseOps,
      int64_t &Offset, bool &OffsetIsScalable,
      LocationSize &Width) const override;
  bool getBaseAndOffsetPosition(const MachineInstr &MI, unsigned &BasePos,
                                unsigned &OffsetPos) const override;
  bool getIncrementValue(const MachineInstr &MI, int &Value) const override;
  bool areMemAccessesTriviallyDisjoint(const MachineInstr &MIa,
                                       const MachineInstr &MIb) const override;

  std::unique_ptr<PipelinerLoopInfo>
  analyzeLoopForPipelining(MachineBasicBlock *LoopBB) const override;

  // Branch analysis. Cond holds the branch opcode and its register operands.
  bool analyzeBranch(MachineBasicBlock &MBB, MachineBasicBlock *&TBB,
                     MachineBasicBlock *&FBB,
                     SmallVectorImpl<MachineOperand> &Cond,
                     bool AllowModify = false) const override;
  unsigned removeBranch(MachineBasicBlock &MBB,
                        int *BytesRemoved = nullptr) const override;
  unsigned insertBranch(MachineBasicBlock &MBB, MachineBasicBlock *TBB,
                        MachineBasicBlock *FBB, ArrayRef<MachineOperand> Cond,
                        const DebugLoc &DL,
                        int *BytesAdded = nullptr) const override;
  bool
  reverseBranchCondition(SmallVectorImpl<MachineOperand> &Cond) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_VU_VUINSTRINFO_H
