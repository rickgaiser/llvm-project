//===-- VUSubtarget.cpp - VU subtarget information ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "VUSubtarget.h"
#include "MCTargetDesc/VUMCTargetDesc.h"

using namespace llvm;

#define DEBUG_TYPE "vu-subtarget"

#define GET_SUBTARGETINFO_TARGET_DESC
#define GET_SUBTARGETINFO_CTOR
#include "VUGenSubtargetInfo.inc"

VUSubtarget &VUSubtarget::initializeSubtargetDependencies(StringRef CPU,
                                                          StringRef FS) {
  if (CPU.empty())
    CPU = "generic";
  ParseSubtargetFeatures(CPU, /*TuneCPU=*/CPU, FS);
  return *this;
}

VUSubtarget::VUSubtarget(const Triple &TT, StringRef CPU, StringRef FS,
                         const TargetMachine &TM)
    : VUGenSubtargetInfo(TT, CPU, /*TuneCPU=*/CPU, FS),
      InstrInfo(initializeSubtargetDependencies(CPU, FS)), TLInfo(TM, *this),
      InstrItins(getInstrItineraryForCPU(CPU.empty() ? "generic" : CPU)) {}
