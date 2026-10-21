//===-- VUTargetMachine.h - Define TargetMachine for VU ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_VU_VUTARGETMACHINE_H
#define LLVM_LIB_TARGET_VU_VUTARGETMACHINE_H

#include "VUSubtarget.h"
#include "llvm/CodeGen/CodeGenTargetMachineImpl.h"
#include <optional>

namespace llvm {

class VUTargetMachine : public CodeGenTargetMachineImpl {
  std::unique_ptr<TargetLoweringObjectFile> TLOF;
  mutable StringMap<std::unique_ptr<VUSubtarget>> SubtargetMap;

public:
  VUTargetMachine(const Target &T, const Triple &TT, StringRef CPU,
                  StringRef FS, const TargetOptions &Options,
                  std::optional<Reloc::Model> RM,
                  std::optional<CodeModel::Model> CM, CodeGenOptLevel OL,
                  bool JIT);
  ~VUTargetMachine() override;

  const VUSubtarget *getSubtargetImpl(const Function &F) const override;
  TargetPassConfig *createPassConfig(PassManagerBase &PM) override;
  TargetLoweringObjectFile *getObjFileLowering() const override {
    return TLOF.get();
  }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_VU_VUTARGETMACHINE_H
