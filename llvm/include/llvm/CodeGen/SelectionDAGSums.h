//===- SelectionDAGSums.h - The terms of sums in a SelectionDAG -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The terms of a sum of adds and subtracts, for the targets that compute it
// in an accumulator: a chain of multiply-adds such as MULA, MADDA..., MADD
// accumulates one term after another.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_SELECTIONDAGSUMS_H
#define LLVM_CODEGEN_SELECTIONDAGSUMS_H

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/SelectionDAGNodes.h"
#include "llvm/Support/Compiler.h"
#include <optional>

namespace llvm {

/// A term of a sum: a value that the sum adds, or subtracts if Neg.
struct SumTerm {
  SDValue V;
  bool Neg = false;
};

/// The terms of a sum, in the order of the evaluation.
struct SumTerms {
  SmallVector<SumTerm, 8> Terms;
  /// The adds and subtracts of the sum, the root first.
  SmallVector<SDNode *, 8> Nodes;
  /// Every add and subtract has a term as an operand, the sum so far as the
  /// other one (the first operand of a subtract): accumulating the terms in
  /// their order gives the same result.
  bool Linear = true;
  /// Every add and subtract may be reassociated.
  bool Reassoc = true;
};

/// Collects the terms of the sum \p Root. \p GetNode returns false (an
/// add) or true (a subtract) for a value that is part of the sum, such as an
/// add that only the sum reads, and std::nullopt for a term; it is also
/// asked for \p Root. Depth first, with the operand that is a sum first: an
/// add is commutative. With \p Flatten false, only the sum so far is
/// followed, and the other operand of each add or subtract is a term, as
/// in a chain (s + t1) - t2.
LLVM_ABI void
collectSumTerms(SDValue Root,
                function_ref<std::optional<bool>(SDValue)> GetNode,
                bool Flatten, SumTerms &S);

} // end namespace llvm

#endif // LLVM_CODEGEN_SELECTIONDAGSUMS_H
