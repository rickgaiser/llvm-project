//===-- DVPVU.h - Top-level interface for DVPVU -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the entry points for global functions defined in the
// LLVM DVPVU back-end.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_DVPVU_DVPVU_H
#define LLVM_LIB_TARGET_DVPVU_DVPVU_H

#include "llvm/Target/TargetMachine.h"

namespace llvm {

class DVPVUTargetMachine;
class FunctionPass;
class PassRegistry;

FunctionPass *createDVPVUISelDag(DVPVUTargetMachine &TM,
                                  CodeGenOptLevel OptLevel);

void initializeDVPVUDAGToDAGISelLegacyPass(PassRegistry &);

} // end namespace llvm

#endif // LLVM_LIB_TARGET_DVPVU_DVPVU_H
