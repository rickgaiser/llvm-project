//===-- DVPVUTargetMachine.cpp - Define TargetMachine for DVPVU -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements the info about DVPVU target spec.
//
//===----------------------------------------------------------------------===//

#include "DVPVUTargetMachine.h"
#include "DVPVU.h"
#include "DVPVUMachineFunctionInfo.h"
#include "DVPVUTargetObjectFile.h"
#include "TargetInfo/DVPVUTargetInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FormattedStream.h"

using namespace llvm;

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeDVPVUTarget() {
  RegisterTargetMachine<DVPVUTargetMachine> X(getTheDVPVUTarget());
  auto &PR = *PassRegistry::getPassRegistry();
  initializeDVPVUDAGToDAGISelLegacyPass(PR);
}

static std::string computeDataLayout() {
  // e = little endian
  // m:e = ELF mangling
  // p:16:16 = 16-bit pointers, 16-bit aligned (VU has 4KB/16KB addressable)
  // i16:16 = 16-bit integers, 16-bit aligned
  // f32:32 = 32-bit floats, 32-bit aligned
  // v128:128 = 128-bit vectors, 128-bit aligned
  // n16:32 = native 16 and 32 bit integers
  return "e-m:e-p:16:16-i16:16-f32:32-v128:128-n16:32";
}

static Reloc::Model getEffectiveRelocModel(std::optional<Reloc::Model> RM) {
  // Default to static relocation model for VU code
  return RM.value_or(Reloc::Static);
}

DVPVUTargetMachine::DVPVUTargetMachine(const Target &T, const Triple &TT,
                                         StringRef CPU, StringRef FS,
                                         const TargetOptions &Options,
                                         std::optional<Reloc::Model> RM,
                                         std::optional<CodeModel::Model> CM,
                                         CodeGenOptLevel OL, bool JIT)
    : CodeGenTargetMachineImpl(T, computeDataLayout(), TT, CPU, FS, Options,
                               getEffectiveRelocModel(RM),
                               CM.value_or(CodeModel::Small), OL),
      TLOF(std::make_unique<DVPVUTargetObjectFile>()),
      Subtarget(TT, std::string(CPU), std::string(FS), *this) {
  initAsmInfo();
}

DVPVUTargetMachine::~DVPVUTargetMachine() = default;

namespace {
class DVPVUPassConfig : public TargetPassConfig {
public:
  DVPVUPassConfig(DVPVUTargetMachine &TM, PassManagerBase &PM)
      : TargetPassConfig(TM, PM) {}

  DVPVUTargetMachine &getDVPVUTargetMachine() const {
    return getTM<DVPVUTargetMachine>();
  }

  bool addInstSelector() override;
  void addPreEmitPass() override;
};
} // end anonymous namespace

TargetPassConfig *DVPVUTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new DVPVUPassConfig(*this, PM);
}

bool DVPVUPassConfig::addInstSelector() {
  addPass(createDVPVUISelDag(getDVPVUTargetMachine(), getOptLevel()));
  return false;
}

void DVPVUPassConfig::addPreEmitPass() {
  // TODO: Add VLIW packetizer pass here
  // addPass(createDVPVUVLIWPacketizerPass());
}

MachineFunctionInfo *DVPVUTargetMachine::createMachineFunctionInfo(
    BumpPtrAllocator &Allocator, const Function &F,
    const TargetSubtargetInfo *STI) const {
  return DVPVUMachineFunctionInfo::create<DVPVUMachineFunctionInfo>(Allocator,
                                                                      F, STI);
}
