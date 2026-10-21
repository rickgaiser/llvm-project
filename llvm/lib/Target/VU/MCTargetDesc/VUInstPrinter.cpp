//===-- VUInstPrinter.cpp - Convert VU MCInst to assembly syntax ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "VUInstPrinter.h"
#include "MCTargetDesc/VUMCTargetDesc.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "asm-printer"

#include "VUGenAsmWriter.inc"

void VUInstPrinter::printRegName(raw_ostream &O, MCRegister Reg) {
  O << getRegisterName(Reg);
}

void VUInstPrinter::printHalf(const MCInst &MI, uint64_t Address,
                              unsigned Flags, raw_ostream &O) {
  std::string Str;
  raw_string_ostream OS(Str);
  printInstruction(&MI, Address, OS);
  StringRef S = StringRef(Str).ltrim('\t');

  // The generated printer separates the mnemonic from the operands with a tab.
  auto [Mnemonic, Operands] = S.split('\t');
  O << Mnemonic;
  if (Flags & VU::FlagE)
    O << "[e]";
  if (Flags & VU::FlagM)
    O << "[m]";
  if (Flags & VU::FlagD)
    O << "[d]";
  if (Flags & VU::FlagT)
    O << "[t]";
  if (!Operands.empty())
    O << '\t' << Operands;
}

void VUInstPrinter::printInst(const MCInst *MI, uint64_t Address,
                              StringRef Annot, const MCSubtargetInfo &STI,
                              raw_ostream &O) {
  if (MI->getOpcode() != VU::BUNDLE) {
    printInstruction(MI, Address, O);
    printAnnotation(O, Annot);
    return;
  }

  // A micro instruction word: "upper[flags] operands <tab> lower operands".
  unsigned Flags = MI->getOperand(VU::BundleFlagsOp).getImm();
  O << '\t';
  printHalf(*MI->getOperand(VU::BundleUpperOp).getInst(), Address, Flags, O);
  O << '\t';
  printHalf(*MI->getOperand(VU::BundleLowerOp).getInst(), Address, 0, O);
  printAnnotation(O, Annot);
}

void VUInstPrinter::printOperand(const MCInst *MI, unsigned OpNo,
                                 raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);
  if (MO.isReg()) {
    printRegName(O, MO.getReg());
    return;
  }
  if (MO.isImm()) {
    O << MO.getImm();
    return;
  }
  assert(MO.isExpr() && "Unknown operand kind");
  MAI.printExpr(O, *MO.getExpr());
}

void VUInstPrinter::printDest(const MCInst *MI, unsigned OpNo, raw_ostream &O) {
  // An empty mask (e.g. in a zero padding word) prints as a lone '.', like
  // dvp-objdump does.
  unsigned Mask = MI->getOperand(OpNo).getImm();
  O << '.';
  if (Mask & 8)
    O << 'x';
  if (Mask & 4)
    O << 'y';
  if (Mask & 2)
    O << 'z';
  if (Mask & 1)
    O << 'w';
}

void VUInstPrinter::printField(const MCInst *MI, unsigned OpNo,
                               raw_ostream &O) {
  O << "xyzw"[MI->getOperand(OpNo).getImm() & 3];
}

void VUInstPrinter::printHex32(const MCInst *MI, unsigned OpNo,
                               raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);
  if (!MO.isImm()) {
    printOperand(MI, OpNo, O);
    return;
  }
  O << format_hex(static_cast<uint32_t>(MO.getImm()), 10);
}

void VUInstPrinter::printBranchTarget(const MCInst *MI, uint64_t Address,
                                      unsigned OpNo, raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);
  if (!MO.isImm()) {
    printOperand(MI, OpNo, O);
    return;
  }

  // The offset is in micro instruction words, relative to the next word.
  int64_t Offset = MO.getImm();
  if (PrintBranchImmAsAddress)
    O << formatHex(static_cast<uint64_t>(Address + 8 + Offset * 8));
  else
    O << Offset;
}
