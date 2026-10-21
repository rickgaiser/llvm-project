//===-- VUSubtarget.h - VU subtarget information ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_VU_VUSUBTARGET_H
#define LLVM_LIB_TARGET_VU_VUSUBTARGET_H

#include "VUFrameLowering.h"
#include "VUISelLowering.h"
#include "VUInstrInfo.h"
#include "VUSelectionDAGInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

#define GET_SUBTARGETINFO_HEADER
#include "VUGenSubtargetInfo.inc"

namespace llvm {

class VUSubtarget : public VUGenSubtargetInfo {
#define GET_SUBTARGETINFO_MACRO(ATTRIBUTE, DEFAULT, GETTER)                    \
  bool ATTRIBUTE = DEFAULT;
#include "VUGenSubtargetInfo.inc"

  VUInstrInfo InstrInfo;
  VUFrameLowering FrameLowering;
  VUTargetLowering TLInfo;
  VUSelectionDAGInfo TSInfo;
  InstrItineraryData InstrItins;

  VUSubtarget &initializeSubtargetDependencies(StringRef CPU, StringRef FS);

public:
  VUSubtarget(const Triple &TT, StringRef CPU, StringRef FS,
              const TargetMachine &TM);

  /// Parses features string setting specified subtarget options.
  void ParseSubtargetFeatures(StringRef CPU, StringRef TuneCPU, StringRef FS);

  bool hasVU1() const { return HasVU1; }

  // Schedule before and after register allocation, then bundle upper and
  // lower instructions into micro instruction words (VUPacketizer).
  bool enableMachineScheduler() const override { return true; }
  bool enablePostRAScheduler() const override { return true; }

  const InstrItineraryData *getInstrItineraryData() const override {
    return &InstrItins;
  }

  const VUInstrInfo *getInstrInfo() const override { return &InstrInfo; }
  const VURegisterInfo *getRegisterInfo() const override {
    return &InstrInfo.getRegisterInfo();
  }
  const TargetFrameLowering *getFrameLowering() const override {
    return &FrameLowering;
  }
  const VUTargetLowering *getTargetLowering() const override { return &TLInfo; }
  const SelectionDAGTargetInfo *getSelectionDAGInfo() const override {
    return &TSInfo;
  }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_VU_VUSUBTARGET_H
