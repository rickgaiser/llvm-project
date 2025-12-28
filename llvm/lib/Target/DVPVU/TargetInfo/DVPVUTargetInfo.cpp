//===-- DVPVUTargetInfo.cpp - DVPVU Target Implementation -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TargetInfo/DVPVUTargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

Target &llvm::getTheDVPVUTarget() {
  static Target TheDVPVUTarget;
  return TheDVPVUTarget;
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeDVPVUTargetInfo() {
  RegisterTarget<Triple::dvpvu> X(getTheDVPVUTarget(), "dvpvu",
                                  "PS2 Vector Unit", "DVPVU");
}
