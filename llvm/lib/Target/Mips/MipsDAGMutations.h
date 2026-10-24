//===- MipsDAGMutations.h - Mips ScheduleDAG mutations ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MIPS_MIPSDAGMUTATIONS_H
#define LLVM_LIB_TARGET_MIPS_MIPSDAGMUTATIONS_H

#include "llvm/CodeGen/ScheduleDAGMutation.h"
#include <memory>

namespace llvm {

/// Keep the loads from the same base register in ascending offset order
/// during post-RA scheduling.
std::unique_ptr<ScheduleDAGMutation> createMipsLoadOrderDAGMutation();

} // end namespace llvm

#endif // LLVM_LIB_TARGET_MIPS_MIPSDAGMUTATIONS_H
