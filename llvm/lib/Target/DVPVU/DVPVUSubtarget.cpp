//===-- DVPVUSubtarget.cpp - DVPVU Subtarget Information ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the DVPVU specific subclass of TargetSubtargetInfo.
//
//===----------------------------------------------------------------------===//

#include "DVPVUSubtarget.h"
#include "DVPVU.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define DEBUG_TYPE "dvpvu-subtarget"

#define GET_SUBTARGETINFO_ENUM
#define GET_SUBTARGETINFO_TARGET_DESC
#define GET_SUBTARGETINFO_CTOR
#include "DVPVUGenSubtargetInfo.inc"

void DVPVUSubtarget::anchor() {}

DVPVUSubtarget &
DVPVUSubtarget::initializeSubtargetDependencies(StringRef CPU, StringRef FS) {
  std::string CPUName = std::string(CPU);
  if (CPUName.empty())
    CPUName = "vu0";

  ParseSubtargetFeatures(CPUName, CPUName, FS);

  // VU1 has EFU by default
  if (IsVU1 && !HasEFU)
    HasEFU = true;

  return *this;
}

DVPVUSubtarget::DVPVUSubtarget(const Triple &TT, StringRef CPU, StringRef FS,
                                 const TargetMachine &TM)
    : DVPVUGenSubtargetInfo(TT, CPU, /*TuneCPU*/ CPU, FS),
      InstrInfo(initializeSubtargetDependencies(CPU, FS)),
      FrameLowering(),
      TLInfo(TM, *this),
      RegInfo() {}
