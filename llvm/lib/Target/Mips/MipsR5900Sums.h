//===- MipsR5900Sums.h - The terms of sums in machine code ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The terms of a sum of adds and subtracts in SSA machine code, for the R5900
// accumulators (MipsR5900FPUAccChain, MipsR5900LoopAcc), as
// llvm/CodeGen/SelectionDAGSums.h is for instruction selection.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MIPS_MIPSR5900SUMS_H
#define LLVM_LIB_TARGET_MIPS_MIPSR5900SUMS_H

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/Register.h"
#include <optional>

namespace llvm {

class MachineInstr;
class MachineRegisterInfo;

/// A term of a sum: a value that the sum adds, or subtracts if Neg.
struct MachineSumTerm {
  Register Reg;
  bool Neg = false;
};

/// An add (Sub false) or subtract of the operands L and R.
struct MachineSumNode {
  Register L, R;
  bool Sub = false;
};

/// The terms of a sum, in the order of the evaluation.
struct MachineSumTerms {
  SmallVector<MachineSumTerm, 8> Terms;
  /// The adds and subtracts of the sum, the root first.
  SmallVector<MachineInstr *, 8> Nodes;
  /// Every add and subtract has a term as an operand, the sum so far as the
  /// other one (the first operand of a subtract): accumulating the terms in
  /// their order gives the same result.
  bool Linear = true;
  /// Every add and subtract may be reassociated.
  bool Reassoc = true;
};

/// Collects the terms of the sum \p Root, depth first with the operand that
/// is a sum first (an add is commutative). \p GetNode returns the operands
/// of a value that is part of the sum, such as an add that only the sum
/// reads, and std::nullopt for a term; it is also asked for \p Root.
/// \p IsSumSoFar tells a term that is the sum of an earlier step, such as
/// the PHI of a loop, which goes first like a sum.
void collectMachineSumTerms(
    Register Root, const MachineRegisterInfo &MRI,
    function_ref<std::optional<MachineSumNode>(Register)> GetNode,
    function_ref<bool(Register)> IsSumSoFar, MachineSumTerms &S);

} // end namespace llvm

#endif // LLVM_LIB_TARGET_MIPS_MIPSR5900SUMS_H
