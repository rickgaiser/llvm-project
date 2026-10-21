//===- MipsR5900IssueModel.h - R5900 instruction issue model ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Simulates in which cycle the R5900 issues each instruction of a sequence.
//
// The R5900 issues the instructions of an aligned 8-byte block (measured):
//   - the two instructions of a block issue in the same cycle if the second
//     is ready, does not depend on the first and their resources fit,
//   - otherwise the first issues alone and the second issues by itself when
//     it is ready; it never issues together with the first instruction of the
//     next block.
// The latencies and resources come from the R5900 scheduling model.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MIPS_MIPSR5900ISSUEMODEL_H
#define LLVM_LIB_TARGET_MIPS_MIPSR5900ISSUEMODEL_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

namespace llvm {

class MachineInstr;
class TargetRegisterInfo;
class TargetSchedModel;
struct MCSchedClassDesc;

class MipsR5900IssueModel {
public:
  MipsR5900IssueModel(const TargetSchedModel &SchedModel,
                      const TargetRegisterInfo &TRI);

  /// Ends the current block. The next instruction is in slot \p Slot (0 or 1)
  /// of a new 8-byte block, for example the target of a taken branch.
  void startBlock(unsigned Slot);

  /// Returns the slot of the next instruction in its 8-byte block.
  unsigned getSlot() const { return Slot; }

  /// Returns the cycle in which \p MI would issue as the next instruction.
  unsigned getIssueCycle(const MachineInstr &MI) const;

  /// Returns true if \p MI would issue together with the first instruction of
  /// its block.
  bool wouldPair(const MachineInstr &MI) const;

  /// Issues \p MI as the next instruction and returns its issue cycle.
  unsigned issue(const MachineInstr &MI);

  /// Returns the first cycle in which the next 8-byte block can issue.
  unsigned getNextBlockCycle() const { return NextBlockCycle; }

private:
  struct RegDef {
    const MachineInstr *MI;
    unsigned OpIdx;
    unsigned Cycle;
  };

  const TargetSchedModel *SchedModel;
  const TargetRegisterInfo *TRI;

  /// The first unit of each processor resource in Busy; groups have none.
  SmallVector<unsigned, 16> FirstUnit;
  /// The cycle in which each resource unit becomes free.
  SmallVector<unsigned, 16> Busy;
  /// Busy before the first instruction of the current block issued.
  SmallVector<unsigned, 16> BusyBeforeSlot0;

  /// The last definition of each register unit.
  DenseMap<unsigned, RegDef> Defs;

  unsigned Slot = 0;
  unsigned NextBlockCycle = 0;
  /// The first instruction of the current block, if it was issued.
  const MachineInstr *Slot0MI = nullptr;
  unsigned Slot0Cycle = 0;

  unsigned getReadyCycle(const MachineInstr &MI) const;
  bool dependsOn(const MachineInstr &MI, const MachineInstr &DefMI) const;
  bool reserve(const MCSchedClassDesc *SC, unsigned Cycle,
               SmallVectorImpl<unsigned> &Units) const;
  bool fitsWithSlot0(const MCSchedClassDesc *SC) const;
  unsigned getResourceCycle(const MCSchedClassDesc *SC, unsigned Cycle,
                            bool Pair) const;
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_MIPS_MIPSR5900ISSUEMODEL_H
