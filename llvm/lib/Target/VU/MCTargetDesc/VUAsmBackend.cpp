//===-- VUAsmBackend.cpp - VU assembler backend ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUMCTargetDesc.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

namespace {
class VUAsmBackend : public MCAsmBackend {
  uint8_t OSABI;

public:
  VUAsmBackend(uint8_t OSABI)
      : MCAsmBackend(endianness::little), OSABI(OSABI) {}

  MCFixupKindInfo getFixupKindInfo(MCFixupKind Kind) const override;
  void applyFixup(const MCFragment &F, const MCFixup &Fixup,
                  const MCValue &Target, uint8_t *Data, uint64_t Value,
                  bool IsResolved) override;
  bool writeNopData(raw_ostream &OS, uint64_t Count,
                    const MCSubtargetInfo *STI) const override;

  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override {
    return createVUELFObjectWriter(OSABI);
  }
};
} // namespace

MCFixupKindInfo VUAsmBackend::getFixupKindInfo(MCFixupKind Kind) const {
  const static MCFixupKindInfo Infos[VU::NumTargetFixupKinds] = {
      // name               offset bits flags
      {"fixup_vu_pcrel11", 0, 11, 0},
  };

  if (Kind < FirstTargetFixupKind)
    return MCAsmBackend::getFixupKindInfo(Kind);
  assert(unsigned(Kind - FirstTargetFixupKind) < VU::NumTargetFixupKinds &&
         "Invalid kind!");
  return Infos[Kind - FirstTargetFixupKind];
}

void VUAsmBackend::applyFixup(const MCFragment &F, const MCFixup &Fixup,
                              const MCValue &Target, uint8_t *Data,
                              uint64_t Value, bool IsResolved) {
  // For a relocation, Value becomes the implicit (REL) addend.
  maybeAddReloc(F, Fixup, Target, Value, IsResolved);

  switch (Fixup.getKind()) {
  case FK_Data_1:
  case FK_Data_2:
  case FK_Data_4:
  case FK_Data_8: {
    unsigned Size = getFixupKindInfo(Fixup.getKind()).TargetSize / 8;
    for (unsigned I = 0; I != Size; ++I)
      Data[I] |= uint8_t(Value >> (I * 8));
    return;
  }
  case VU::fixup_vu_pcrel11: {
    // Branches are relative to the next micro instruction word. For an
    // unresolved target this stores the addend the same way dvp-as does.
    int64_t Offset = static_cast<int64_t>(Value) - 8;
    if (Offset % 8 != 0) {
      getContext().reportError(Fixup.getLoc(),
                               "branch target is not 8-byte aligned");
      return;
    }
    Offset /= 8;
    if (!isInt<11>(Offset)) {
      getContext().reportError(Fixup.getLoc(), "branch target out of range");
      return;
    }
    uint32_t Field = static_cast<uint32_t>(Offset) & 0x7ff;
    Data[0] |= uint8_t(Field);
    Data[1] |= uint8_t(Field >> 8);
    return;
  }
  default:
    llvm_unreachable("Unknown fixup kind!");
  }
}

bool VUAsmBackend::writeNopData(raw_ostream &OS, uint64_t Count,
                                const MCSubtargetInfo *STI) const {
  // Like dvp-as, pad with zeros. An all-zero word has empty destination masks
  // in both halves, so it does not change any register.
  OS.write_zeros(Count);
  return true;
}

MCAsmBackend *llvm::createVUAsmBackend(const Target &T,
                                       const MCSubtargetInfo &STI,
                                       const MCRegisterInfo &MRI,
                                       const MCTargetOptions &Options) {
  uint8_t OSABI =
      MCELFObjectTargetWriter::getOSABI(STI.getTargetTriple().getOS());
  return new VUAsmBackend(OSABI);
}
