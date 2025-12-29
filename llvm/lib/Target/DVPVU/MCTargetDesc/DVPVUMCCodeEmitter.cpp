//===-- DVPVUMCCodeEmitter.cpp - Convert DVPVU code to machine code -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the DVPVUMCCodeEmitter class.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/DVPVUMCTargetDesc.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/EndianStream.h"
#include <cassert>
#include <cstdint>

using namespace llvm;

#define DEBUG_TYPE "mccodeemitter"

STATISTIC(MCNumEmitted, "Number of MC instructions emitted");

namespace {

class DVPVUMCCodeEmitter : public MCCodeEmitter {
  const MCInstrInfo &MCII;
  MCContext &Ctx;

public:
  DVPVUMCCodeEmitter(const MCInstrInfo &MCII, MCContext &C)
      : MCII(MCII), Ctx(C) {}
  DVPVUMCCodeEmitter(const DVPVUMCCodeEmitter &) = delete;
  void operator=(const DVPVUMCCodeEmitter &) = delete;
  ~DVPVUMCCodeEmitter() override = default;

  // getBinaryCodeForInstr - TableGen'erated function for getting the
  // binary encoding for an instruction.
  uint64_t getBinaryCodeForInstr(const MCInst &Inst,
                                 SmallVectorImpl<MCFixup> &Fixups,
                                 const MCSubtargetInfo &SubtargetInfo) const;

  // getMachineOpValue - Return binary encoding of operand.
  unsigned getMachineOpValue(const MCInst &Inst, const MCOperand &MCOp,
                             SmallVectorImpl<MCFixup> &Fixups,
                             const MCSubtargetInfo &SubtargetInfo) const;

  // Get encoding for various operand types
  unsigned getDestMaskOpValue(const MCInst &Inst, unsigned OpNo,
                               SmallVectorImpl<MCFixup> &Fixups,
                               const MCSubtargetInfo &STI) const;

  unsigned getMemVIOpValue(const MCInst &Inst, unsigned OpNo,
                            SmallVectorImpl<MCFixup> &Fixups,
                            const MCSubtargetInfo &STI) const;

  unsigned getBranchTargetOpValue(const MCInst &Inst, unsigned OpNo,
                                   SmallVectorImpl<MCFixup> &Fixups,
                                   const MCSubtargetInfo &STI) const;

  void encodeInstruction(const MCInst &Inst, SmallVectorImpl<char> &CB,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &SubtargetInfo) const override;

  void reportUnsupportedInst(const MCInst &MI) const {
    // For now just silently ignore unsupported instructions
  }
};

} // end anonymous namespace

// Get DVPVU register number from LLVM register number
static unsigned getDVPVURegisterNumbering(unsigned Reg) {
  // VF registers: VF0-VF31 map to 0-31
  // VI registers: VI0-VI15 map to 0-15
  // The register numbers should already be encoded correctly from TableGen
  switch (Reg) {
  // Vector float registers
  case DVPVU::VF0: return 0;
  case DVPVU::VF1: return 1;
  case DVPVU::VF2: return 2;
  case DVPVU::VF3: return 3;
  case DVPVU::VF4: return 4;
  case DVPVU::VF5: return 5;
  case DVPVU::VF6: return 6;
  case DVPVU::VF7: return 7;
  case DVPVU::VF8: return 8;
  case DVPVU::VF9: return 9;
  case DVPVU::VF10: return 10;
  case DVPVU::VF11: return 11;
  case DVPVU::VF12: return 12;
  case DVPVU::VF13: return 13;
  case DVPVU::VF14: return 14;
  case DVPVU::VF15: return 15;
  case DVPVU::VF16: return 16;
  case DVPVU::VF17: return 17;
  case DVPVU::VF18: return 18;
  case DVPVU::VF19: return 19;
  case DVPVU::VF20: return 20;
  case DVPVU::VF21: return 21;
  case DVPVU::VF22: return 22;
  case DVPVU::VF23: return 23;
  case DVPVU::VF24: return 24;
  case DVPVU::VF25: return 25;
  case DVPVU::VF26: return 26;
  case DVPVU::VF27: return 27;
  case DVPVU::VF28: return 28;
  case DVPVU::VF29: return 29;
  case DVPVU::VF30: return 30;
  case DVPVU::VF31: return 31;
  // Vector integer registers
  case DVPVU::VI0: return 0;
  case DVPVU::VI1: return 1;
  case DVPVU::VI2: return 2;
  case DVPVU::VI3: return 3;
  case DVPVU::VI4: return 4;
  case DVPVU::VI5: return 5;
  case DVPVU::VI6: return 6;
  case DVPVU::VI7: return 7;
  case DVPVU::VI8: return 8;
  case DVPVU::VI9: return 9;
  case DVPVU::VI10: return 10;
  case DVPVU::VI11: return 11;
  case DVPVU::VI12: return 12;
  case DVPVU::VI13: return 13;
  case DVPVU::VI14: return 14;
  case DVPVU::VI15: return 15;
  default:
    return 0;
  }
}

unsigned DVPVUMCCodeEmitter::getMachineOpValue(
    const MCInst &Inst, const MCOperand &MCOp, SmallVectorImpl<MCFixup> &Fixups,
    const MCSubtargetInfo &SubtargetInfo) const {
  if (MCOp.isReg())
    return getDVPVURegisterNumbering(MCOp.getReg());
  if (MCOp.isImm())
    return static_cast<unsigned>(MCOp.getImm());

  // MCOp must be an expression
  assert(MCOp.isExpr());
  // For now, just return 0 for expressions - fixups will handle it
  return 0;
}

unsigned DVPVUMCCodeEmitter::getDestMaskOpValue(
    const MCInst &Inst, unsigned OpNo, SmallVectorImpl<MCFixup> &Fixups,
    const MCSubtargetInfo &STI) const {
  const MCOperand &Op = Inst.getOperand(OpNo);
  assert(Op.isImm() && "Expected immediate operand for dest mask");
  return Op.getImm() & 0xF;
}

unsigned DVPVUMCCodeEmitter::getMemVIOpValue(
    const MCInst &Inst, unsigned OpNo, SmallVectorImpl<MCFixup> &Fixups,
    const MCSubtargetInfo &STI) const {
  const MCOperand &Base = Inst.getOperand(OpNo);
  const MCOperand &Offset = Inst.getOperand(OpNo + 1);

  unsigned BaseReg = 0;
  if (Base.isReg())
    BaseReg = getDVPVURegisterNumbering(Base.getReg());

  unsigned Off = 0;
  if (Offset.isImm())
    Off = Offset.getImm() & 0x7FF;

  return (BaseReg << 11) | Off;
}

unsigned DVPVUMCCodeEmitter::getBranchTargetOpValue(
    const MCInst &Inst, unsigned OpNo, SmallVectorImpl<MCFixup> &Fixups,
    const MCSubtargetInfo &STI) const {
  const MCOperand &Op = Inst.getOperand(OpNo);
  if (Op.isImm())
    return Op.getImm() & 0x7FF;

  // For expressions, we need to create a fixup
  assert(Op.isExpr() && "Expected expression operand");
  // TODO: Add proper fixup handling
  return 0;
}

void DVPVUMCCodeEmitter::encodeInstruction(
    const MCInst &Inst, SmallVectorImpl<char> &CB,
    SmallVectorImpl<MCFixup> &Fixups,
    const MCSubtargetInfo &SubtargetInfo) const {
  // Get the 64-bit encoding
  uint64_t Bits = getBinaryCodeForInstr(Inst, Fixups, SubtargetInfo);

  // VU is little-endian, emit the 64-bit instruction
  support::endian::write<uint64_t>(CB, Bits, llvm::endianness::little);

  ++MCNumEmitted;
}

#include "DVPVUGenMCCodeEmitter.inc"

MCCodeEmitter *llvm::createDVPVUMCCodeEmitter(const MCInstrInfo &MCII,
                                               MCContext &Ctx) {
  return new DVPVUMCCodeEmitter(MCII, Ctx);
}
