//===-- DVPVUMachineFunctionInfo.h - DVPVU machine func info ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares DVPVU-specific per-machine-function information.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_DVPVU_DVPVUMACHINEFUNCTIONINFO_H
#define LLVM_LIB_TARGET_DVPVU_DVPVUMACHINEFUNCTIONINFO_H

#include "llvm/CodeGen/MachineFunction.h"

namespace llvm {

class DVPVUMachineFunctionInfo : public MachineFunctionInfo {
  virtual void anchor();

  // Size of the data memory used for spills
  unsigned DataMemorySize = 0;

  // Whether this function uses EFU instructions
  bool UsesEFU = false;

  // Whether this function uses the Q register
  bool UsesQ = false;

  // Whether this function uses the P register
  bool UsesP = false;

public:
  DVPVUMachineFunctionInfo() = default;

  DVPVUMachineFunctionInfo(const Function &F, const TargetSubtargetInfo *STI)
      : DataMemorySize(0), UsesEFU(false), UsesQ(false), UsesP(false) {}

  MachineFunctionInfo *
  clone(BumpPtrAllocator &Allocator, MachineFunction &DestMF,
        const DenseMap<MachineBasicBlock *, MachineBasicBlock *> &Src2DstMBB)
      const override {
    return DestMF.cloneInfo<DVPVUMachineFunctionInfo>(*this);
  }

  unsigned getDataMemorySize() const { return DataMemorySize; }
  void setDataMemorySize(unsigned Size) { DataMemorySize = Size; }

  bool usesEFU() const { return UsesEFU; }
  void setUsesEFU(bool Uses = true) { UsesEFU = Uses; }

  bool usesQ() const { return UsesQ; }
  void setUsesQ(bool Uses = true) { UsesQ = Uses; }

  bool usesP() const { return UsesP; }
  void setUsesP(bool Uses = true) { UsesP = Uses; }
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_DVPVU_DVPVUMACHINEFUNCTIONINFO_H
