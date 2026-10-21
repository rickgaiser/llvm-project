//===-- VUMCAsmInfo.cpp - VU asm properties -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "VUMCAsmInfo.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

VUMCAsmInfo::VUMCAsmInfo(const Triple &TT, const MCTargetOptions &Options)
    : MCAsmInfoELF(Options) {
  CodePointerSize = 2;
  CalleeSaveStackSlotSize = 16;
  // VCL and dvp-as sources use ';' for comments, so it can't also separate
  // statements.
  CommentString = ";";
  SeparatorString = "%%";
  // ".align N" aligns to 2^N bytes, as in dvp-as.
  AlignmentIsInBytes = false;
  MinInstAlignment = 8;
  UsesELFSectionDirectiveForBSS = true;
  SupportsDebugInformation = false;
}
