//===-- DVPVUMCTargetDesc.cpp - DVPVU Target Descriptions -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides DVPVU specific target descriptions.
//
//===----------------------------------------------------------------------===//

#include "DVPVUMCTargetDesc.h"
#include "DVPVUInstPrinter.h"
#include "DVPVUMCAsmInfo.h"
#include "TargetInfo/DVPVUTargetInfo.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/TargetParser/Triple.h"
#include <cstdint>
#include <string>

#define GET_INSTRINFO_MC_DESC
#define ENABLE_INSTR_PREDICATE_VERIFIER
#include "DVPVUGenInstrInfo.inc"

#define GET_SUBTARGETINFO_MC_DESC
#include "DVPVUGenSubtargetInfo.inc"

#define GET_REGINFO_MC_DESC
#include "DVPVUGenRegisterInfo.inc"

using namespace llvm;

static MCInstrInfo *createDVPVUMCInstrInfo() {
  MCInstrInfo *X = new MCInstrInfo();
  InitDVPVUMCInstrInfo(X);
  return X;
}

static MCRegisterInfo *createDVPVUMCRegisterInfo(const Triple & /*TT*/) {
  MCRegisterInfo *X = new MCRegisterInfo();
  // Use VI15 as return address register (link register convention)
  InitDVPVUMCRegisterInfo(X, DVPVU::VI15);
  return X;
}

static MCSubtargetInfo *
createDVPVUMCSubtargetInfo(const Triple &TT, StringRef CPU, StringRef FS) {
  std::string CPUName = std::string(CPU);
  if (CPUName.empty())
    CPUName = "vu0";

  return createDVPVUMCSubtargetInfoImpl(TT, CPUName, /*TuneCPU*/ CPUName, FS);
}

static MCStreamer *createMCStreamer(const Triple &T, MCContext &Context,
                                    std::unique_ptr<MCAsmBackend> &&MAB,
                                    std::unique_ptr<MCObjectWriter> &&OW,
                                    std::unique_ptr<MCCodeEmitter> &&Emitter) {
  if (!T.isOSBinFormatELF())
    llvm_unreachable("OS not supported");

  return createELFStreamer(Context, std::move(MAB), std::move(OW),
                           std::move(Emitter));
}

static MCInstPrinter *createDVPVUMCInstPrinter(const Triple & /*T*/,
                                                unsigned SyntaxVariant,
                                                const MCAsmInfo &MAI,
                                                const MCInstrInfo &MII,
                                                const MCRegisterInfo &MRI) {
  if (SyntaxVariant == 0)
    return new DVPVUInstPrinter(MAI, MII, MRI);
  return nullptr;
}

static MCRelocationInfo *createDVPVUElfRelocation(const Triple &TheTriple,
                                                   MCContext &Ctx) {
  return createMCRelocationInfo(TheTriple, Ctx);
}

namespace {

class DVPVUMCInstrAnalysis : public MCInstrAnalysis {
public:
  explicit DVPVUMCInstrAnalysis(const MCInstrInfo *Info)
      : MCInstrAnalysis(Info) {}

  bool evaluateBranch(const MCInst &Inst, uint64_t Addr, uint64_t Size,
                      uint64_t &Target) const override {
    if (Inst.getNumOperands() == 0)
      return false;
    if (!isConditionalBranch(Inst) && !isUnconditionalBranch(Inst) &&
        !isCall(Inst))
      return false;

    // PC-relative branch
    int64_t Imm = Inst.getOperand(0).getImm();
    // VU instructions are 64-bit (8 bytes), offset is in instruction units
    Target = Addr + Size + (Imm * 8);
    return true;
  }
};

} // end anonymous namespace

static MCInstrAnalysis *createDVPVUInstrAnalysis(const MCInstrInfo *Info) {
  return new DVPVUMCInstrAnalysis(Info);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeDVPVUTargetMC() {
  // Register the MC asm info.
  RegisterMCAsmInfo<DVPVUMCAsmInfo> X(getTheDVPVUTarget());

  // Register the MC instruction info.
  TargetRegistry::RegisterMCInstrInfo(getTheDVPVUTarget(),
                                      createDVPVUMCInstrInfo);

  // Register the MC register info.
  TargetRegistry::RegisterMCRegInfo(getTheDVPVUTarget(),
                                    createDVPVUMCRegisterInfo);

  // Register the MC subtarget info.
  TargetRegistry::RegisterMCSubtargetInfo(getTheDVPVUTarget(),
                                          createDVPVUMCSubtargetInfo);

  // Register the MC code emitter
  TargetRegistry::RegisterMCCodeEmitter(getTheDVPVUTarget(),
                                        createDVPVUMCCodeEmitter);

  // Register the ASM Backend
  TargetRegistry::RegisterMCAsmBackend(getTheDVPVUTarget(),
                                       createDVPVUAsmBackend);

  // Register the MCInstPrinter.
  TargetRegistry::RegisterMCInstPrinter(getTheDVPVUTarget(),
                                        createDVPVUMCInstPrinter);

  // Register the ELF streamer.
  TargetRegistry::RegisterELFStreamer(getTheDVPVUTarget(), createMCStreamer);

  // Register the MC relocation info.
  TargetRegistry::RegisterMCRelocationInfo(getTheDVPVUTarget(),
                                           createDVPVUElfRelocation);

  // Register the MC instruction analyzer.
  TargetRegistry::RegisterMCInstrAnalysis(getTheDVPVUTarget(),
                                          createDVPVUInstrAnalysis);
}
