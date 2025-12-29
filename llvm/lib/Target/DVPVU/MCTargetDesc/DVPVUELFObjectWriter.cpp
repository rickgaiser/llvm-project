//===-- DVPVUELFObjectWriter.cpp - DVPVU ELF Writer -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/DVPVUMCTargetDesc.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {

class DVPVUELFObjectWriter : public MCELFObjectTargetWriter {
public:
  explicit DVPVUELFObjectWriter(uint8_t OSABI)
      : MCELFObjectTargetWriter(/*Is64Bit=*/false, OSABI,
                                 /*ELF Machine type - using EM_NONE for custom*/
                                 ELF::EM_NONE,
                                 /*HasRelocationAddend=*/true) {}

  ~DVPVUELFObjectWriter() override = default;

protected:
  unsigned getRelocType(const MCFixup &Fixup, const MCValue &Target,
                        bool IsPCRel) const override {
    // For now, return 0 (no relocation)
    return 0;
  }
};

} // end anonymous namespace

std::unique_ptr<MCObjectTargetWriter>
llvm::createDVPVUELFObjectWriter(uint8_t OSABI) {
  return std::make_unique<DVPVUELFObjectWriter>(OSABI);
}
