//===-- VUELFObjectWriter.cpp - VU ELF writer -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// VU objects are 32-bit little-endian MIPS ELF files with REL relocations, as
// produced by the DVP port of GNU as (dvp-as). This lets them be linked into
// Emotion Engine programs.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUMCTargetDesc.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCValue.h"

using namespace llvm;

namespace {
/// Relocation types of the DVP binutils port (EM_MIPS).
enum : unsigned {
  R_MIPS_DVP_11_PCREL = 120,
};

class VUELFObjectWriter : public MCELFObjectTargetWriter {
public:
  VUELFObjectWriter(uint8_t OSABI)
      : MCELFObjectTargetWriter(/*Is64Bit=*/false, OSABI, ELF::EM_MIPS,
                                /*HasRelocationAddend=*/false) {}

protected:
  unsigned getRelocType(const MCFixup &Fixup, const MCValue &Target,
                        bool IsPCRel) const override;
};
} // namespace

unsigned VUELFObjectWriter::getRelocType(const MCFixup &Fixup,
                                         const MCValue &Target,
                                         bool IsPCRel) const {
  switch (Fixup.getKind()) {
  case VU::fixup_vu_pcrel11:
    return R_MIPS_DVP_11_PCREL;
  case FK_Data_2:
    if (!IsPCRel)
      return ELF::R_MIPS_16;
    break;
  case FK_Data_4:
    if (!IsPCRel)
      return ELF::R_MIPS_32;
    break;
  default:
    break;
  }
  reportError(Fixup.getLoc(), "unsupported relocation type");
  return ELF::R_MIPS_NONE;
}

std::unique_ptr<MCObjectTargetWriter>
llvm::createVUELFObjectWriter(uint8_t OSABI) {
  return std::make_unique<VUELFObjectWriter>(OSABI);
}
