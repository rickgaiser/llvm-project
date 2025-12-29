//===-- DVPVUSubtarget.h - Define Subtarget for DVPVU -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the DVPVU specific subclass of TargetSubtargetInfo.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_DVPVU_DVPVUSUBTARGET_H
#define LLVM_LIB_TARGET_DVPVU_DVPVUSUBTARGET_H

#include "DVPVUFrameLowering.h"
#include "DVPVUISelLowering.h"
#include "DVPVUInstrInfo.h"
#include "DVPVURegisterInfo.h"
#include "DVPVUSelectionDAGInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Target/TargetMachine.h"

#define GET_SUBTARGETINFO_HEADER
#include "DVPVUGenSubtargetInfo.inc"

namespace llvm {

class StringRef;

class DVPVUSubtarget : public DVPVUGenSubtargetInfo {
  virtual void anchor();

  DVPVUInstrInfo InstrInfo;
  DVPVUFrameLowering FrameLowering;
  DVPVUTargetLowering TLInfo;
  DVPVUSelectionDAGInfo TSInfo;
  DVPVURegisterInfo RegInfo;

  // Subtarget features
  bool IsVU1 = false;
  bool HasEFU = false;

  DVPVUSubtarget &initializeSubtargetDependencies(StringRef CPU, StringRef FS);

public:
  DVPVUSubtarget(const Triple &TT, StringRef CPU, StringRef FS,
                  const TargetMachine &TM);

  void ParseSubtargetFeatures(StringRef CPU, StringRef TuneCPU, StringRef FS);

  const DVPVUInstrInfo *getInstrInfo() const override { return &InstrInfo; }
  const DVPVUFrameLowering *getFrameLowering() const override {
    return &FrameLowering;
  }
  const DVPVURegisterInfo *getRegisterInfo() const override {
    return &RegInfo;
  }
  const DVPVUTargetLowering *getTargetLowering() const override {
    return &TLInfo;
  }
  const DVPVUSelectionDAGInfo *getSelectionDAGInfo() const override {
    return &TSInfo;
  }

  bool isVU1() const { return IsVU1; }
  bool hasEFU() const { return HasEFU; }
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_DVPVU_DVPVUSUBTARGET_H
