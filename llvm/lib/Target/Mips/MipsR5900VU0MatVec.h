//===- MipsR5900VU0MatVec.h - R5900 VU0 matrix times vector -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MIPS_MIPSR5900VU0MATVEC_H
#define LLVM_LIB_TARGET_MIPS_MIPSR5900VU0MATVEC_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class MipsTargetMachine;

/// Rewrite rows of dot products of a loop-invariant matrix with one vector of
/// 4 floats in memory, such as a transform written with scalar structs, as a
/// VU0 matrix times vector: the matrix columns are built before the loop and
/// each iteration is a vector load and a broadcast multiply-add per column.
class MipsR5900VU0MatVecPass
    : public OptionalPassInfoMixin<MipsR5900VU0MatVecPass> {
  const MipsTargetMachine &TM;

public:
  MipsR5900VU0MatVecPass(const MipsTargetMachine &TM) : TM(TM) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_MIPS_MIPSR5900VU0MATVEC_H
