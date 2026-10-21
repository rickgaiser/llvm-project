//===-- VUTargetObjectFile.cpp - VU object file lowering ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "VUTargetObjectFile.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCSectionELF.h"

using namespace llvm;

MCSection *VUTargetObjectFile::SelectSectionForGlobal(
    const GlobalObject *GO, SectionKind Kind, const TargetMachine &TM) const {
  if (Kind.isText()) {
    MCSection *Sec = getContext().getELFSection(
        ".vutext", ELF::SHT_PROGBITS, ELF::SHF_ALLOC | ELF::SHF_EXECINSTR);
    Sec->ensureMinAlignment(Align(8));
    return Sec;
  }
  return TargetLoweringObjectFileELF::SelectSectionForGlobal(GO, Kind, TM);
}
