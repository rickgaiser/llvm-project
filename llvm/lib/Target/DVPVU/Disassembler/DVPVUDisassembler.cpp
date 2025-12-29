//===-- DVPVUDisassembler.cpp - Disassembler for DVPVU --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/DVPVUMCTargetDesc.h"
#include "TargetInfo/DVPVUTargetInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDecoder.h"
#include "llvm/MC/MCDecoderOps.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;
using namespace llvm::MCD;

#define DEBUG_TYPE "dvpvu-disassembler"

typedef MCDisassembler::DecodeStatus DecodeStatus;

namespace {

class DVPVUDisassembler : public MCDisassembler {
public:
  DVPVUDisassembler(const MCSubtargetInfo &STI, MCContext &Ctx)
      : MCDisassembler(STI, Ctx) {}

  ~DVPVUDisassembler() override = default;

  DecodeStatus getInstruction(MCInst &Instr, uint64_t &Size,
                              ArrayRef<uint8_t> Bytes, uint64_t Address,
                              raw_ostream &CStream) const override;
};

} // end anonymous namespace

static MCDisassembler *createDVPVUDisassembler(const Target &T,
                                                const MCSubtargetInfo &STI,
                                                MCContext &Ctx) {
  return new DVPVUDisassembler(STI, Ctx);
}

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeDVPVUDisassembler() {
  TargetRegistry::RegisterMCDisassembler(getTheDVPVUTarget(),
                                          createDVPVUDisassembler);
}

// Decoder functions - must be defined before the generated tables include

static DecodeStatus DecodeVFRegsRegisterClass(MCInst &Inst, uint64_t RegNo,
                                               uint64_t Address,
                                               const MCDisassembler *Decoder) {
  if (RegNo > 31)
    return MCDisassembler::Fail;

  MCRegister Reg = DVPVU::VF0 + RegNo;
  Inst.addOperand(MCOperand::createReg(Reg));
  return MCDisassembler::Success;
}

static DecodeStatus DecodeVIRegsRegisterClass(MCInst &Inst, uint64_t RegNo,
                                               uint64_t Address,
                                               const MCDisassembler *Decoder) {
  if (RegNo > 15)
    return MCDisassembler::Fail;

  MCRegister Reg = DVPVU::VI0 + RegNo;
  Inst.addOperand(MCOperand::createReg(Reg));
  return MCDisassembler::Success;
}

static DecodeStatus DecodeVFRegsNoVF0RegisterClass(MCInst &Inst, uint64_t RegNo,
                                                    uint64_t Address,
                                                    const MCDisassembler *Decoder) {
  if (RegNo == 0 || RegNo > 31)
    return MCDisassembler::Fail;

  MCRegister Reg = DVPVU::VF0 + RegNo;
  Inst.addOperand(MCOperand::createReg(Reg));
  return MCDisassembler::Success;
}

static DecodeStatus DecodeVIRegsNoVI0RegisterClass(MCInst &Inst, uint64_t RegNo,
                                                    uint64_t Address,
                                                    const MCDisassembler *Decoder) {
  if (RegNo == 0 || RegNo > 15)
    return MCDisassembler::Fail;

  MCRegister Reg = DVPVU::VI0 + RegNo;
  Inst.addOperand(MCOperand::createReg(Reg));
  return MCDisassembler::Success;
}

static DecodeStatus DecodeACCRegsRegisterClass(MCInst &Inst, uint64_t RegNo,
                                                uint64_t Address,
                                                const MCDisassembler *Decoder) {
  Inst.addOperand(MCOperand::createReg(DVPVU::ACC));
  return MCDisassembler::Success;
}

static DecodeStatus DecodeDestMask(MCInst &Inst, uint64_t Imm,
                                    uint64_t Address,
                                    const MCDisassembler *Decoder) {
  Inst.addOperand(MCOperand::createImm(Imm & 0xF));
  return MCDisassembler::Success;
}

static DecodeStatus DecodeBroadcast(MCInst &Inst, uint64_t Imm,
                                     uint64_t Address,
                                     const MCDisassembler *Decoder) {
  Inst.addOperand(MCOperand::createImm(Imm & 0x3));
  return MCDisassembler::Success;
}

static DecodeStatus DecodeField(MCInst &Inst, uint64_t Imm,
                                 uint64_t Address,
                                 const MCDisassembler *Decoder) {
  Inst.addOperand(MCOperand::createImm(Imm & 0x3));
  return MCDisassembler::Success;
}

static DecodeStatus decodeSImm11(MCInst &Inst, uint64_t Imm,
                                  uint64_t Address,
                                  const MCDisassembler *Decoder) {
  // Branch targets are 11-bit signed offsets
  int64_t Offset = SignExtend64<11>(Imm);
  Inst.addOperand(MCOperand::createImm(Offset));
  return MCDisassembler::Success;
}

static DecodeStatus decodeUImm5(MCInst &Inst, uint64_t Imm,
                                 uint64_t Address,
                                 const MCDisassembler *Decoder) {
  Inst.addOperand(MCOperand::createImm(Imm & 0x1F));
  return MCDisassembler::Success;
}

static DecodeStatus decodeUImm15(MCInst &Inst, uint64_t Imm,
                                  uint64_t Address,
                                  const MCDisassembler *Decoder) {
  Inst.addOperand(MCOperand::createImm(Imm & 0x7FFF));
  return MCDisassembler::Success;
}

static DecodeStatus DecodeMemVI(MCInst &Inst, uint64_t Val,
                                 uint64_t Address,
                                 const MCDisassembler *Decoder) {
  // Memory operand: base reg (5 bits) + offset (11 bits)
  unsigned BaseReg = (Val >> 11) & 0x1F;
  int64_t Offset = SignExtend64<11>(Val & 0x7FF);

  if (BaseReg > 15)
    return MCDisassembler::Fail;

  MCRegister Reg = DVPVU::VI0 + BaseReg;
  Inst.addOperand(MCOperand::createReg(Reg));
  Inst.addOperand(MCOperand::createImm(Offset));
  return MCDisassembler::Success;
}

// Decoder tables generated by TableGen
#include "DVPVUGenDisassemblerTables.inc"

DecodeStatus DVPVUDisassembler::getInstruction(MCInst &Instr, uint64_t &Size,
                                                ArrayRef<uint8_t> Bytes,
                                                uint64_t Address,
                                                raw_ostream &CStream) const {
  // VU instructions are 64-bit (8 bytes)
  if (Bytes.size() < 8) {
    Size = 0;
    return MCDisassembler::Fail;
  }

  Size = 8;

  // Read the 64-bit instruction (little-endian)
  uint64_t Insn = support::endian::read64le(Bytes.data());

  // Try to decode using TableGen-generated decoder
  DecodeStatus Result = decodeInstruction(DecoderTableDVPVU64, Instr, Insn,
                                           Address, this, STI);

  if (Result == MCDisassembler::Fail) {
    Instr.clear();
    return MCDisassembler::Fail;
  }

  return Result;
}
