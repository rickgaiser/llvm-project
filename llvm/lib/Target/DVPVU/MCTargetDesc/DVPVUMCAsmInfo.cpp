//===-- DVPVUMCAsmInfo.cpp - DVPVU asm properties -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the declarations of the DVPVUMCAsmInfo properties.
//
//===----------------------------------------------------------------------===//

#include "DVPVUMCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

void DVPVUMCAsmInfo::anchor() {}

DVPVUMCAsmInfo::DVPVUMCAsmInfo(const Triple & /*TheTriple*/,
                               const MCTargetOptions &Options) {
  // VU is little endian
  IsLittleEndian = true;

  PrivateGlobalPrefix = ".L";
  WeakRefDirective = "\t.weak\t";

  // Use ';' as comment string (common in VU assembly)
  CommentString = ";";

  // Target supports emission of debugging information.
  SupportsDebugInformation = true;

  // VU instructions are 64-bit (8 bytes)
  MinInstAlignment = 8;

  // Code pointer size is 32-bit (instruction memory addresses)
  CodePointerSize = 4;

  // Data pointer size is also 32-bit
  CalleeSaveStackSlotSize = 4;

  // Use ELF section directive for BSS
  UsesELFSectionDirectiveForBSS = true;

  // No exception handling for VU
  ExceptionsType = ExceptionHandling::None;
}
