//===-- VUDisassembler.cpp - Disassembler for VU micro code ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Every micro instruction word is 8 bytes: the lower instruction followed by
// the upper instruction. Both are decoded into a single BUNDLE MCInst.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUMCTargetDesc.h"
#include "TargetInfo/VUTargetInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDecoder.h"
#include "llvm/MC/MCDecoderOps.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Endian.h"

using namespace llvm;
using namespace llvm::MCD;

#define DEBUG_TYPE "vu-disassembler"

using DecodeStatus = MCDisassembler::DecodeStatus;

namespace {
class VUDisassembler : public MCDisassembler {
public:
  VUDisassembler(const MCSubtargetInfo &STI, MCContext &Ctx)
      : MCDisassembler(STI, Ctx) {}

  DecodeStatus getInstruction(MCInst &Instr, uint64_t &Size,
                              ArrayRef<uint8_t> Bytes, uint64_t Address,
                              raw_ostream &CStream) const override;
};
} // end anonymous namespace

static MCDisassembler *createVUDisassembler(const Target &T,
                                            const MCSubtargetInfo &STI,
                                            MCContext &Ctx) {
  return new VUDisassembler(STI, Ctx);
}

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeVUDisassembler() {
  TargetRegistry::RegisterMCDisassembler(getTheVUTarget(),
                                         createVUDisassembler);
}

static DecodeStatus DecodeVFRegisterClass(MCInst &Inst, uint64_t RegNo,
                                          uint64_t Address,
                                          const MCDisassembler *Decoder) {
  if (RegNo > 31)
    return MCDisassembler::Fail;
  Inst.addOperand(MCOperand::createReg(VU::VF0 + RegNo));
  return MCDisassembler::Success;
}

static DecodeStatus DecodeVIRegisterClass(MCInst &Inst, uint64_t RegNo,
                                          uint64_t Address,
                                          const MCDisassembler *Decoder) {
  // The register fields are 5 bits wide, but there are only 16 VI registers.
  if (RegNo > 15)
    return MCDisassembler::Fail;
  Inst.addOperand(MCOperand::createReg(VU::VI0 + RegNo));
  return MCDisassembler::Success;
}

// ACC and Q are not encoded.
static DecodeStatus DecodeACCRRegisterClass(MCInst &Inst,
                                            const MCDisassembler *Decoder) {
  Inst.addOperand(MCOperand::createReg(VU::ACC));
  return MCDisassembler::Success;
}

static DecodeStatus DecodeQRRegisterClass(MCInst &Inst,
                                          const MCDisassembler *Decoder) {
  Inst.addOperand(MCOperand::createReg(VU::Q));
  return MCDisassembler::Success;
}

template <unsigned N>
static DecodeStatus decodeSImmOperand(MCInst &Inst, uint64_t Imm,
                                      uint64_t Address,
                                      const MCDisassembler *Decoder) {
  assert(isUInt<N>(Imm) && "Invalid immediate");
  Inst.addOperand(MCOperand::createImm(SignExtend64<N>(Imm)));
  return MCDisassembler::Success;
}

#include "VUGenDisassemblerTables.inc"

DecodeStatus VUDisassembler::getInstruction(MCInst &MI, uint64_t &Size,
                                            ArrayRef<uint8_t> Bytes,
                                            uint64_t Address,
                                            raw_ostream &CS) const {
  if (Bytes.size() < 8) {
    Size = 0;
    return MCDisassembler::Fail;
  }
  Size = 8;

  uint32_t LowerBits = support::endian::read32le(Bytes.data());
  uint32_t UpperBits = support::endian::read32le(Bytes.data() + 4);
  unsigned Flags = UpperBits >> VU::BundleFlagsShift;
  UpperBits &= (1u << VU::BundleFlagsShift) - 1;

  MCInst *Upper = getContext().createMCInst();
  MCInst *Lower = getContext().createMCInst();

  DecodeStatus Result = decodeInstruction(DecoderTableUpper32, *Upper,
                                          UpperBits, Address, this, STI);
  if (Result == MCDisassembler::Fail)
    return MCDisassembler::Fail;

  DecodeStatus LowerResult;
  if (Flags & VU::FlagI) {
    // The lower word is the immediate of LOI; the I flag is implied by it.
    LowerResult = decodeInstruction(DecoderTableLOI32, *Lower, LowerBits,
                                    Address, this, STI);
    Flags &= ~VU::FlagI;
  } else if (LowerBits == VU::LowerNopEncoding) {
    Lower->setOpcode(VU::LNOP);
    LowerResult = MCDisassembler::Success;
  } else {
    LowerResult = decodeInstruction(DecoderTableLower32, *Lower, LowerBits,
                                    Address, this, STI);
  }
  if (LowerResult == MCDisassembler::Fail)
    return MCDisassembler::Fail;
  if (LowerResult == MCDisassembler::SoftFail)
    Result = MCDisassembler::SoftFail;

  MI.setOpcode(VU::BUNDLE);
  MI.addOperand(MCOperand::createImm(Flags));
  MI.addOperand(MCOperand::createInst(Upper));
  MI.addOperand(MCOperand::createInst(Lower));
  return Result;
}
