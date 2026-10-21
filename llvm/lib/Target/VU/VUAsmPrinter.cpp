//===-- VUAsmPrinter.cpp - VU LLVM assembly writer ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Every MachineInstr becomes one micro instruction word (a BUNDLE): an upper
// instruction is paired with a lower NOP and vice versa.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUInstPrinter.h"
#include "MCTargetDesc/VUMCTargetDesc.h"
#include "TargetInfo/VUTargetInfo.h"
#include "VU.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

#define DEBUG_TYPE "asm-printer"

namespace {
class VUAsmPrinter : public AsmPrinter {
public:
  static char ID;

  explicit VUAsmPrinter(TargetMachine &TM, std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer), ID) {}

  StringRef getPassName() const override { return "VU Assembly Printer"; }
  void emitInstruction(const MachineInstr *MI) override;

private:
  void lowerInstruction(const MachineInstr &MI, MCInst &Inst) const;
  void emitBundle(const MCInst &Upper, const MCInst &Lower, unsigned Flags);
};
} // namespace

void VUAsmPrinter::lowerInstruction(const MachineInstr &MI,
                                    MCInst &Inst) const {
  Inst.setOpcode(MI.getOpcode());
  for (const MachineOperand &MO : MI.operands()) {
    switch (MO.getType()) {
    case MachineOperand::MO_Register:
      if (MO.isImplicit())
        continue;
      Inst.addOperand(MCOperand::createReg(MO.getReg()));
      break;
    case MachineOperand::MO_Immediate:
      Inst.addOperand(MCOperand::createImm(MO.getImm()));
      break;
    case MachineOperand::MO_MachineBasicBlock:
      Inst.addOperand(MCOperand::createExpr(
          MCSymbolRefExpr::create(MO.getMBB()->getSymbol(), OutContext)));
      break;
    case MachineOperand::MO_RegisterMask:
      break;
    default:
      report_fatal_error("VU: unsupported machine operand");
    }
  }
}

void VUAsmPrinter::emitBundle(const MCInst &Upper, const MCInst &Lower,
                              unsigned Flags) {
  MCInst *U = OutContext.createMCInst();
  *U = Upper;
  MCInst *L = OutContext.createMCInst();
  *L = Lower;

  MCInst Bundle;
  Bundle.setOpcode(VU::BUNDLE);
  Bundle.addOperand(MCOperand::createImm(Flags));
  Bundle.addOperand(MCOperand::createInst(U));
  Bundle.addOperand(MCOperand::createInst(L));
  EmitToStreamer(*OutStreamer, Bundle);
}

void VUAsmPrinter::emitInstruction(const MachineInstr *MI) {
  MCInst UpperNop, LowerNop;
  UpperNop.setOpcode(VU::NOP);
  LowerNop.setOpcode(VU::LNOP);

  if (MI->getOpcode() == VU::PROGRAM_END) {
    // The E bit ends the program after the next word (its delay slot).
    emitBundle(UpperNop, LowerNop, VU::FlagE);
    emitBundle(UpperNop, LowerNop, 0);
    return;
  }

  MCInst Inst;
  lowerInstruction(*MI, Inst);
  if (VU::isLowerInstr(MI->getDesc()))
    emitBundle(UpperNop, Inst, 0);
  else
    emitBundle(Inst, LowerNop, 0);
}

char VUAsmPrinter::ID = 0;

INITIALIZE_PASS(VUAsmPrinter, "vu-asm-printer", "VU Assembly Printer", false,
                false)

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeVUAsmPrinter() {
  RegisterAsmPrinter<VUAsmPrinter> X(getTheVUTarget());
}
