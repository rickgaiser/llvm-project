//===-- DVPVUInstPrinter.cpp - Convert DVPVU MCInst to asm syntax ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This class prints a DVPVU MCInst to a .s file.
//
//===----------------------------------------------------------------------===//

#include "DVPVUInstPrinter.h"
#include "MCTargetDesc/DVPVUMCTargetDesc.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define DEBUG_TYPE "asm-printer"

// Include the auto-generated portion of the assembly writer.
#define PRINT_ALIAS_INSTR
#include "DVPVUGenAsmWriter.inc"

void DVPVUInstPrinter::printRegName(raw_ostream &OS, MCRegister Reg) {
  OS << StringRef(getRegisterName(Reg)).lower();
}

void DVPVUInstPrinter::printInst(const MCInst *MI, uint64_t Address,
                                  StringRef Annot, const MCSubtargetInfo &STI,
                                  raw_ostream &O) {
  if (!printAliasInstr(MI, Address, O))
    printInstruction(MI, Address, O);
  printAnnotation(O, Annot);
}

void DVPVUInstPrinter::printOperand(const MCInst *MI, unsigned OpNo,
                                     raw_ostream &O) {
  const MCOperand &Op = MI->getOperand(OpNo);
  if (Op.isReg()) {
    printRegName(O, Op.getReg());
  } else if (Op.isImm()) {
    O << Op.getImm();
  } else if (Op.isExpr()) {
    MAI.printExpr(O, *Op.getExpr());
  } else {
    llvm_unreachable("Unknown operand type");
  }
}

void DVPVUInstPrinter::printDestMask(const MCInst *MI, unsigned OpNo,
                                      raw_ostream &O) {
  unsigned Mask = MI->getOperand(OpNo).getImm();
  O << ".";
  if (Mask & 0x8) O << "x";
  if (Mask & 0x4) O << "y";
  if (Mask & 0x2) O << "z";
  if (Mask & 0x1) O << "w";
}

void DVPVUInstPrinter::printBroadcast(const MCInst *MI, unsigned OpNo,
                                       raw_ostream &O) {
  unsigned BC = MI->getOperand(OpNo).getImm();
  switch (BC) {
  case 0: O << "x"; break;
  case 1: O << "y"; break;
  case 2: O << "z"; break;
  case 3: O << "w"; break;
  default: llvm_unreachable("Invalid broadcast component");
  }
}

void DVPVUInstPrinter::printField(const MCInst *MI, unsigned OpNo,
                                   raw_ostream &O) {
  unsigned Field = MI->getOperand(OpNo).getImm();
  switch (Field) {
  case 0: O << "x"; break;
  case 1: O << "y"; break;
  case 2: O << "z"; break;
  case 3: O << "w"; break;
  default: llvm_unreachable("Invalid field selector");
  }
}

void DVPVUInstPrinter::printMemVI(const MCInst *MI, unsigned OpNo,
                                   raw_ostream &O) {
  const MCOperand &Base = MI->getOperand(OpNo);
  const MCOperand &Offset = MI->getOperand(OpNo + 1);

  O << Offset.getImm() << "(";
  printRegName(O, Base.getReg());
  O << ")";
}
