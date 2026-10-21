//===-- VUTargetObjectFile.h - VU object file lowering ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_VU_VUTARGETOBJECTFILE_H
#define LLVM_LIB_TARGET_VU_VUTARGETOBJECTFILE_H

#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"

namespace llvm {

/// Places micro programs in the ".vutext" section, like dvp-as.
class VUTargetObjectFile : public TargetLoweringObjectFileELF {
public:
  MCSection *SelectSectionForGlobal(const GlobalObject *GO, SectionKind Kind,
                                    const TargetMachine &TM) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_VU_VUTARGETOBJECTFILE_H
