//===-- VU.h - Top-level interface for the VU target ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_VU_VU_H
#define LLVM_LIB_TARGET_VU_VU_H

#include "llvm/IR/PassManager.h"
#include "llvm/Support/CodeGen.h"

namespace llvm {
class FunctionPass;
class ModulePass;
class PassRegistry;
class VUTargetMachine;

FunctionPass *createVUISelDag(VUTargetMachine &TM, CodeGenOptLevel OptLevel);
FunctionPass *createVUDelaySlotFillerPass();
FunctionPass *createVUPacketizerPass();
FunctionPass *createVUFoldQPass();
FunctionPass *createVUPackConstantsPass();
FunctionPass *createVUFoldQCopiesPass();
FunctionPass *createVUFoldInsertFieldPass();
FunctionPass *createVUHazardsPass();
FunctionPass *createVULoopCopiesPass();
FunctionPass *createVULoopSchedulerPass();
FunctionPass *createVULowerMemoryPass();
ModulePass *createVUEntryLoweringPass();
ModulePass *createVUHLSLAdapterPass();
FunctionPass *createVUNarrowIntPass();
FunctionPass *createVUSinkFieldsPass();

void initializeVUAsmPrinterPass(PassRegistry &);
void initializeVUDAGToDAGISelLegacyPass(PassRegistry &);
void initializeVUDelaySlotFillerPass(PassRegistry &);
void initializeVUPacketizerPass(PassRegistry &);
void initializeVUFoldQPass(PassRegistry &);
void initializeVUPackConstantsPass(PassRegistry &);
void initializeVUFoldQCopiesPass(PassRegistry &);
void initializeVUFoldInsertFieldPass(PassRegistry &);
void initializeVUHazardsPass(PassRegistry &);
void initializeVULoopCopiesPass(PassRegistry &);
void initializeVULoopSchedulerPass(PassRegistry &);
void initializeVULowerMemoryPass(PassRegistry &);
void initializeVUEntryLoweringLegacyPass(PassRegistry &);
void initializeVUHLSLAdapterLegacyPass(PassRegistry &);
void initializeVUNarrowIntPass(PassRegistry &);
void initializeVUSinkFieldsPass(PassRegistry &);

/// Address spaces of the globals of an entry function (VUEntryLowering).
/// Pointers of all of them are byte addresses of VU data memory.
namespace VUAS {
enum : unsigned {
  Memory = 0,
  Uniform = 1,
  Input = 2,
  Output = 3,
  Items = 4,
  Private = 5,
};
} // namespace VUAS

class VUEntryLoweringPass : public RequiredPassInfoMixin<VUEntryLoweringPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

/// clang's HLSL output for SPIR-V as VU entry functions (VUHLSLAdapter.cpp).
class VUHLSLAdapterPass : public RequiredPassInfoMixin<VUHLSLAdapterPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};
} // namespace llvm

#endif // LLVM_LIB_TARGET_VU_VU_H
