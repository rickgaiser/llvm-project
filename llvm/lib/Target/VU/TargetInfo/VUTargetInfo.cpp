//===-- VUTargetInfo.cpp - VU target implementation -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TargetInfo/VUTargetInfo.h"
#include "llvm/MC/TargetRegistry.h"

using namespace llvm;

Target &llvm::getTheVUTarget() {
  static Target TheVUTarget;
  return TheVUTarget;
}

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeVUTargetInfo() {
  RegisterTarget<Triple::vu> X(getTheVUTarget(), "vu",
                               "PlayStation 2 Vector Unit (micro mode)", "VU");
}
