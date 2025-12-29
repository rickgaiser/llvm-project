//===-- DVPVUMCInstLower.h - Lower MachineInstr to MCInst -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the DVPVUMCInstLower class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_DVPVU_DVPVUMCINSTLOWER_H
#define LLVM_LIB_TARGET_DVPVU_DVPVUMCINSTLOWER_H

#include "llvm/Support/Compiler.h"

namespace llvm {

class AsmPrinter;
class MCContext;
class MCInst;
class MCOperand;
class MCSymbol;
class MachineInstr;
class MachineOperand;

class DVPVUMCInstLower {
  MCContext &Ctx;
  AsmPrinter &Printer;

public:
  DVPVUMCInstLower(MCContext &Ctx, AsmPrinter &Printer)
      : Ctx(Ctx), Printer(Printer) {}

  void Lower(const MachineInstr *MI, MCInst &OutMI) const;
  MCOperand LowerOperand(const MachineOperand &MO, unsigned Offset = 0) const;

private:
  MCOperand LowerSymbolOperand(const MachineOperand &MO, MCSymbol *Sym) const;
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_DVPVU_DVPVUMCINSTLOWER_H
