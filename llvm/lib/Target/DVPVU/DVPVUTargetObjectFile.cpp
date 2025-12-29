//===-- DVPVUTargetObjectFile.cpp - DVPVU Object Files --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DVPVUTargetObjectFile.h"
#include "llvm/MC/MCContext.h"

using namespace llvm;

void DVPVUTargetObjectFile::Initialize(MCContext &Ctx,
                                         const TargetMachine &TM) {
  TargetLoweringObjectFileELF::Initialize(Ctx, TM);

  // VU microcode sections
  // Code goes to .vutext, data to .vudata
}
