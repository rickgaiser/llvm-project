//===-- VUMCTargetDesc.cpp - VU target descriptions -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "VUMCTargetDesc.h"
#include "TargetInfo/VUTargetInfo.h"
#include "VUInstPrinter.h"
#include "VUMCAsmInfo.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCELFStreamer.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

#define GET_INSTRINFO_MC_DESC
#include "VUGenInstrInfo.inc"

#define GET_REGINFO_MC_DESC
#include "VUGenRegisterInfo.inc"

#define GET_SUBTARGETINFO_MC_DESC
#include "VUGenSubtargetInfo.inc"

using namespace llvm;

static MCAsmInfo *createVUMCAsmInfo(const MCRegisterInfo &MRI, const Triple &TT,
                                    const MCTargetOptions &Options) {
  return new VUMCAsmInfo(TT, Options);
}

static MCInstrInfo *createVUMCInstrInfo() {
  MCInstrInfo *X = new MCInstrInfo();
  InitVUMCInstrInfo(X);
  return X;
}

static MCInstrAnalysis *createVUMCInstrAnalysis(const MCInstrInfo *Info) {
  return new MCInstrAnalysis(Info);
}

static MCRegisterInfo *createVUMCRegisterInfo(const Triple &TT) {
  MCRegisterInfo *X = new MCRegisterInfo();
  // BAL and JALR conventionally link through VI15.
  InitVUMCRegisterInfo(X, VU::VI15);
  return X;
}

static MCSubtargetInfo *createVUMCSubtargetInfo(const Triple &TT, StringRef CPU,
                                                StringRef FS) {
  if (CPU.empty())
    CPU = "generic";
  return createVUMCSubtargetInfoImpl(TT, CPU, /*TuneCPU=*/CPU, FS);
}

static MCInstPrinter *createVUMCInstPrinter(const Triple &TT,
                                            unsigned SyntaxVariant,
                                            const MCAsmInfo &MAI,
                                            const MCInstrInfo &MII,
                                            const MCRegisterInfo &MRI) {
  return new VUInstPrinter(MAI, MII, MRI);
}

namespace {
/// Sets the ELF header flags that dvp-as uses, so that VU objects can be
/// linked with Emotion Engine (R5900) objects.
class VUTargetELFStreamer : public MCTargetStreamer {
public:
  VUTargetELFStreamer(MCStreamer &S) : MCTargetStreamer(S) {
    ELFObjectWriter &W = static_cast<MCELFStreamer &>(S).getWriter();
    W.setELFHeaderEFlags(ELF::EF_MIPS_ARCH_3 | ELF::EF_MIPS_MACH_5900 |
                         ELF::EF_MIPS_ABI2 | ELF::EF_MIPS_CPIC);
  }
};
} // namespace

static MCTargetStreamer *
createVUObjectTargetStreamer(MCStreamer &S, const MCSubtargetInfo &STI) {
  return new VUTargetELFStreamer(S);
}

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeVUTargetMC() {
  Target &T = getTheVUTarget();
  TargetRegistry::RegisterMCAsmInfo(T, createVUMCAsmInfo);
  TargetRegistry::RegisterMCInstrInfo(T, createVUMCInstrInfo);
  TargetRegistry::RegisterMCRegInfo(T, createVUMCRegisterInfo);
  TargetRegistry::RegisterMCInstrAnalysis(T, createVUMCInstrAnalysis);
  TargetRegistry::RegisterMCSubtargetInfo(T, createVUMCSubtargetInfo);
  TargetRegistry::RegisterMCInstPrinter(T, createVUMCInstPrinter);
  TargetRegistry::RegisterMCCodeEmitter(T, createVUMCCodeEmitter);
  TargetRegistry::RegisterMCAsmBackend(T, createVUAsmBackend);
  TargetRegistry::RegisterObjectTargetStreamer(T, createVUObjectTargetStreamer);
}
