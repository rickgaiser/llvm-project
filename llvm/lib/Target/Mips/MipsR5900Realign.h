//===- MipsR5900Realign.h - R5900 unaligned vector loads --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MIPS_MIPSR5900REALIGN_H
#define LLVM_LIB_TARGET_MIPS_MIPSR5900REALIGN_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class MipsTargetMachine;

/// Rebase unaligned 128-bit integer vector loads at a constant offset from a
/// 16-byte aligned pointer on llvm.ptrmask of that pointer, so that the
/// SelectionDAG sees the alignment and loads the aligned quadwords, which
/// nearby loads share, and shifts them with QFSRV.
class MipsR5900RealignPass
    : public OptionalPassInfoMixin<MipsR5900RealignPass> {
  const MipsTargetMachine &TM;

public:
  MipsR5900RealignPass(const MipsTargetMachine &TM) : TM(TM) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_MIPS_MIPSR5900REALIGN_H
