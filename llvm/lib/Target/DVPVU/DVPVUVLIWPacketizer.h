//===-- DVPVUVLIWPacketizer.h - DVPVU VLIW Packetizer -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the DVPVU VLIW Packetizer pass.
//
// The VU executes 64-bit instruction words containing:
// - Upper (bits 63-32): FMAC operations
// - Lower (bits 31-0): Various operations (IALU, LSU, BRU, etc.)
//
// This pass bundles compatible Upper+Lower instruction pairs together
// and inserts NOPs where needed.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_DVPVU_DVPVUVLIWPACKETIZER_H
#define LLVM_LIB_TARGET_DVPVU_DVPVUVLIWPACKETIZER_H

#include "llvm/CodeGen/MachineFunctionPass.h"

namespace llvm {

class DVPVUInstrInfo;
class TargetInstrInfo;

class DVPVUVLIWPacketizer : public MachineFunctionPass {
public:
  static char ID;

  DVPVUVLIWPacketizer();

  StringRef getPassName() const override { return "DVPVU VLIW Packetizer"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  const DVPVUInstrInfo *TII = nullptr;

  /// Returns true if the instruction uses the Upper (FMAC) slot
  bool isUpperSlot(const MachineInstr &MI) const;

  /// Returns true if the instruction uses the Lower slot
  bool isLowerSlot(const MachineInstr &MI) const;

  /// Check if two instructions can be bundled together
  bool canBundle(const MachineInstr &Upper, const MachineInstr &Lower) const;

  /// Check for data hazards between two instructions
  bool hasDataHazard(const MachineInstr &First,
                     const MachineInstr &Second) const;

  /// Process a basic block and bundle instructions
  bool packetizeMBB(MachineBasicBlock &MBB);
};

/// Create the DVPVU VLIW Packetizer pass
FunctionPass *createDVPVUVLIWPacketizerPass();

/// Initialize the DVPVU VLIW Packetizer pass
void initializeDVPVUVLIWPacketizerPass(PassRegistry &);

} // end namespace llvm

#endif // LLVM_LIB_TARGET_DVPVU_DVPVUVLIWPACKETIZER_H
