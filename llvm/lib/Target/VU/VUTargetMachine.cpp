//===-- VUTargetMachine.cpp - VU target machine ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The VU target currently only provides the MC layer (assembler, disassembler
// and object file emission). Code generation is not implemented yet, so no
// TargetMachine is registered.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/Compiler.h"

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeVUTarget() {}
