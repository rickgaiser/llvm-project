//===-- DVPVUAsmBackend.cpp - DVPVU Assembler Backend ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/DVPVUMCTargetDesc.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {

class DVPVUAsmBackend : public MCAsmBackend {
public:
  DVPVUAsmBackend(const Target &T, const MCSubtargetInfo &STI)
      : MCAsmBackend(llvm::endianness::little) {}

  void applyFixup(const MCFragment &F, const MCFixup &Fixup,
                  const MCValue &Target, uint8_t *Data, uint64_t Value,
                  bool IsResolved) override {
    // TODO: Implement fixup application
  }

  bool fixupNeedsRelaxation(const MCFixup &Fixup,
                            uint64_t Value) const override {
    return false;
  }

  void relaxInstruction(MCInst &Inst,
                        const MCSubtargetInfo &STI) const override {
    llvm_unreachable("DVPVU does not support instruction relaxation");
  }

  bool writeNopData(raw_ostream &OS, uint64_t Count,
                    const MCSubtargetInfo *STI) const override {
    // VU NOP is: Upper = 0x2F000000 (NOP), Lower = 0x80000033 (NOP)
    // Combined: 0x2F000000'80000033
    if (Count % 8 != 0)
      return false;

    uint64_t NOP = 0x2F00000080000033ULL;
    for (uint64_t i = 0; i < Count; i += 8) {
      for (int j = 0; j < 8; ++j)
        OS << static_cast<char>((NOP >> (j * 8)) & 0xFF);
    }
    return true;
  }

  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override {
    return createDVPVUELFObjectWriter(ELF::ELFOSABI_NONE);
  }
};

} // end anonymous namespace

MCAsmBackend *llvm::createDVPVUAsmBackend(const Target &T,
                                           const MCSubtargetInfo &STI,
                                           const MCRegisterInfo &MRI,
                                           const MCTargetOptions &Options) {
  return new DVPVUAsmBackend(T, STI);
}
