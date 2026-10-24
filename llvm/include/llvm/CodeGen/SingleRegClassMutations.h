//===- SingleRegClassMutations.h - Values of one-register classes -*- C++ -*-=//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Schedule DAG mutations for the values of register classes with a single
// register, such as an accumulator: only one of them can be live at a time.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_SINGLEREGCLASSMUTATIONS_H
#define LLVM_CODEGEN_SINGLEREGCLASSMUTATIONS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/CodeGen/ScheduleDAGMutation.h"
#include "llvm/Support/Compiler.h"
#include <memory>

namespace llvm {

class MCRegisterClass;
using TargetRegisterClass = MCRegisterClass;

/// Keeps the live ranges of the virtual registers of each register class
/// with a single register in their input order: an artificial edge from each
/// reader of the values of a chain (such as MULA, MADDA..., MADD) to the
/// instruction that starts the next one, a write that reads no value of the
/// class. The scheduler would otherwise overlap them to hide latencies, and
/// every overlapping value would have to be moved to another register class
/// and back. An instruction that writes the physical register directly ends
/// the value too.
LLVM_ABI std::unique_ptr<ScheduleDAGMutation>
createSingleRegClassOrderMutation();

/// For the MachinePipeliner: a chain of instructions that pass a value of one
/// of \p Classes on (such as MULA, MADDA..., MADD for an accumulator) starts
/// only when all its instructions can follow without waiting for their other
/// operands. Otherwise the start is scheduled as early as possible and the
/// rest later, which keeps the register busy for longer, or puts the chain
/// in two stages.
LLVM_ABI std::unique_ptr<ScheduleDAGMutation> createSingleRegChainStartMutation(
    ArrayRef<const TargetRegisterClass *> Classes);

} // end namespace llvm

#endif // LLVM_CODEGEN_SINGLEREGCLASSMUTATIONS_H
