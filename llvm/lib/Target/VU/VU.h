//===-- VU.h - Top-level interface for the VU target ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_VU_VU_H
#define LLVM_LIB_TARGET_VU_VU_H

#include "llvm/Support/CodeGen.h"

namespace llvm {
class FunctionPass;
class PassRegistry;
class VUTargetMachine;

FunctionPass *createVUISelDag(VUTargetMachine &TM, CodeGenOptLevel OptLevel);
FunctionPass *createVUDelaySlotFillerPass();
FunctionPass *createVUPacketizerPass();
FunctionPass *createVUFoldQPass();
FunctionPass *createVUHazardsPass();

void initializeVUAsmPrinterPass(PassRegistry &);
void initializeVUDAGToDAGISelLegacyPass(PassRegistry &);
void initializeVUDelaySlotFillerPass(PassRegistry &);
void initializeVUPacketizerPass(PassRegistry &);
void initializeVUFoldQPass(PassRegistry &);
void initializeVUHazardsPass(PassRegistry &);
} // namespace llvm

#endif // LLVM_LIB_TARGET_VU_VU_H
