//===-- VUTargetMachine.cpp - Define TargetMachine for VU -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Code generation for VU micro programs. Loops are software pipelined, then
// instructions are scheduled before and after register allocation and paired
// into micro instruction words.
//
//===----------------------------------------------------------------------===//

#include "VUTargetMachine.h"
#include "TargetInfo/VUTargetInfo.h"
#include "VU.h"
#include "VUTargetObjectFile.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/Function.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeVUTarget() {
  RegisterTargetMachine<VUTargetMachine> X(getTheVUTarget());
  PassRegistry &PR = *PassRegistry::getPassRegistry();
  initializeVUAsmPrinterPass(PR);
  initializeVUDAGToDAGISelLegacyPass(PR);
  initializeVUDelaySlotFillerPass(PR);
  initializeVUPacketizerPass(PR);
}

VUTargetMachine::VUTargetMachine(const Target &T, const Triple &TT,
                                 StringRef CPU, StringRef FS,
                                 const TargetOptions &Options,
                                 std::optional<Reloc::Model> RM,
                                 std::optional<CodeModel::Model> CM,
                                 CodeGenOptLevel OL, bool JIT)
    : CodeGenTargetMachineImpl(T, TT, CPU, FS, Options,
                               RM.value_or(Reloc::Static),
                               getEffectiveCodeModel(CM, CodeModel::Small), OL),
      TLOF(std::make_unique<VUTargetObjectFile>()) {
  initAsmInfo();
}

VUTargetMachine::~VUTargetMachine() = default;

const VUSubtarget *VUTargetMachine::getSubtargetImpl(const Function &F) const {
  Attribute CPUAttr = F.getFnAttribute("target-cpu");
  Attribute FSAttr = F.getFnAttribute("target-features");
  std::string CPU =
      CPUAttr.isValid() ? CPUAttr.getValueAsString().str() : TargetCPU;
  std::string FS =
      FSAttr.isValid() ? FSAttr.getValueAsString().str() : TargetFS;

  auto &I = SubtargetMap[CPU + FS];
  if (!I)
    I = std::make_unique<VUSubtarget>(TargetTriple, CPU, FS, *this);
  return I.get();
}

namespace {
class VUPassConfig : public TargetPassConfig {
public:
  VUPassConfig(VUTargetMachine &TM, PassManagerBase &PM)
      : TargetPassConfig(TM, PM) {
    substitutePass(&PostRASchedulerID, &PostMachineSchedulerID);
  }

  VUTargetMachine &getVUTargetMachine() const {
    return getTM<VUTargetMachine>();
  }

  bool addInstSelector() override {
    addPass(createVUISelDag(getVUTargetMachine(), getOptLevel()));
    return false;
  }

  void addPreRegAlloc() override {
    if (getOptLevel() != CodeGenOptLevel::None)
      addPass(&MachinePipelinerID);
  }

  void addPreEmitPass() override {
    if (getOptLevel() != CodeGenOptLevel::None)
      addPass(createVUPacketizerPass());
    addPass(createVUDelaySlotFillerPass());
  }
};
} // namespace

TargetPassConfig *VUTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new VUPassConfig(*this, PM);
}
