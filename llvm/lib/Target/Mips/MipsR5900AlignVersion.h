//===- MipsR5900AlignVersion.h - R5900 versioning on alignment -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MIPS_MIPSR5900ALIGNVERSION_H
#define LLVM_LIB_TARGET_MIPS_MIPSR5900ALIGNVERSION_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class MipsTargetMachine;

/// Version a loop over arrays of unknown alignment on whether they are
/// 16-byte aligned, so that the loop vectorizer can use the aligned 128-bit
/// loads and stores of MMI (LQ, SQ) and VU0 (LQC2, SQC2) in the aligned copy.
class MipsR5900AlignVersionPass
    : public OptionalPassInfoMixin<MipsR5900AlignVersionPass> {
  const MipsTargetMachine &TM;

public:
  MipsR5900AlignVersionPass(const MipsTargetMachine &TM) : TM(TM) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_MIPS_MIPSR5900ALIGNVERSION_H
