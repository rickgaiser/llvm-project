//===- DVPVUInstrInfo.td - DVPVU Instruction defs ----------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file describes the PS2 VU instruction set.
//
// VU Instruction Format:
// - 64-bit VLIW instruction word
// - Upper 32 bits: FMAC operations (vector float math)
// - Lower 32 bits: Various operations (FDIV, LSU, IALU, BRU, EFU)
// - Both execute in parallel
//
// Destination Masking:
// - 4-bit DEST field selects which components (x,y,z,w) are written
// - Bit 3 = x, Bit 2 = y, Bit 1 = z, Bit 0 = w
//
//===----------------------------------------------------------------------===//

include "DVPVUInstrFormats.td"

//===----------------------------------------------------------------------===//
// Operand Definitions
//===----------------------------------------------------------------------===//

// Destination mask operand (xyzw)
def DestMaskOperand : AsmOperandClass {
  let Name = "DestMask";
  let RenderMethod = "addDestMaskOperands";
  let ParserMethod = "parseDestMask";
}

def destmask : Operand<i32> {
  let PrintMethod = "printDestMask";
  let ParserMatchClass = DestMaskOperand;
}

// Broadcast component operand (x, y, z, or w)
def BroadcastOperand : AsmOperandClass {
  let Name = "Broadcast";
  let RenderMethod = "addBroadcastOperands";
  let ParserMethod = "parseBroadcast";
}

def broadcast : Operand<i32> {
  let PrintMethod = "printBroadcast";
  let ParserMatchClass = BroadcastOperand;
}

// Field selector operand (x, y, z, or w for scalar ops)
def FieldSelOperand : AsmOperandClass {
  let Name = "Field";
  let RenderMethod = "addFieldOperands";
  let ParserMethod = "parseField";
}

def fieldsel : Operand<i32> {
  let PrintMethod = "printField";
  let ParserMatchClass = FieldSelOperand;
}

// Memory operand for load/store: offset(VIreg)
def MemVIOperand : AsmOperandClass {
  let Name = "MemVI";
  let RenderMethod = "addMemVIOperands";
  let ParserMethod = "parseMemVI";
}

def memvi : Operand<i32> {
  let PrintMethod = "printMemVI";
  let MIOperandInfo = (ops VIRegs, i32imm);
  let ParserMatchClass = MemVIOperand;
  let DecoderMethod = "DecodeMemVI";
}

// 11-bit signed immediate for branches
def simm11 : Operand<i32> {
  let DecoderMethod = "decodeSImm11";
}

// 5-bit unsigned immediate
def uimm5 : Operand<i32> {
  let DecoderMethod = "decodeUImm5";
}

// 15-bit unsigned immediate
def uimm15 : Operand<i32> {
  let DecoderMethod = "decodeUImm15";
}

//===----------------------------------------------------------------------===//
// SDNode Definitions
//===----------------------------------------------------------------------===//

// Return with glue
def RetGlue : SDNode<"DVPVUISD::RET_GLUE", SDTNone,
                     [SDNPHasChain, SDNPOptInGlue, SDNPVariadic]>;

//===----------------------------------------------------------------------===//
// Addressing Mode Patterns
//===----------------------------------------------------------------------===//

// Address operand: base register + immediate offset (16-bit pointers)
def AddrVI : ComplexPattern<i16, 2, "SelectAddrVI", [frameindex], []>;

//===----------------------------------------------------------------------===//
// Pseudo-instructions for CodeGen
//===----------------------------------------------------------------------===//

// Return pseudo-instruction
// VU uses JR VI15 for return (VI15 is the link register)
let isReturn = 1, isTerminator = 1, isBarrier = 1,
    hasDelaySlot = 1, isCodeGenOnly = 1, Uses = [VI15] in
def RET : DVPVUInst<(outs), (ins),
                    "jr\t$$vi15",
                    [(RetGlue)]>;

//===----------------------------------------------------------------------===//
// Upper Instructions (FMAC Pipeline)
//===----------------------------------------------------------------------===//

// ADD - Vector Add
// Opcodes from binutils dvp-opc.c (VUOP6 values):
// ADD: 0x28, MUL: 0x2a, MAX: 0x2b, SUB: 0x2c, MSUB: 0x2d, OPMSUB: 0x2e, MINI: 0x2f
// MADD: 0x29
// Note: Bit 5=1 distinguishes regular from broadcast instructions
let Itinerary = IIC_FMAC in {

def ADDvv : UpperInst<0b101000,  // 0x28
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "add$dest\t$vd, $vs, $vt",
    []>;

def SUBvv : UpperInst<0b101100,  // 0x2c
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "sub$dest\t$vd, $vs, $vt",
    []>;

def MULvv : UpperInst<0b101010,  // 0x2a
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "mul$dest\t$vd, $vs, $vt",
    []>;

def MADDvv : UpperInst<0b101001,  // 0x29
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "madd$dest\t$vd, $vs, $vt",
    []>;  // Uses ACC

def MSUBvv : UpperInst<0b101101,  // 0x2d
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "msub$dest\t$vd, $vs, $vt",
    []>;

def MAXvv : UpperInst<0b101011,  // 0x2b
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "max$dest\t$vd, $vs, $vt",
    []>;

def MINIvv : UpperInst<0b101111,  // 0x2f
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "mini$dest\t$vd, $vs, $vt",
    []>;

def ABSv : UpperInst2Op<0b111101,  // 0x3d (0x1fd is 11-bit, low 6 bits = 0b111101)
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs),
    "abs$dest\t$vd, $vs",
    []>;

// Upper NOP
def NOP_UPPER : UpperNopInst<0b101111,
    (outs), (ins),
    "nop",
    []> {
  let Itinerary = IIC_NOP;
}

} // Itinerary = IIC_FMAC

//===----------------------------------------------------------------------===//
// Upper Broadcast Instructions
//===----------------------------------------------------------------------===//
// Broadcast instructions: multiply/add/sub with a single component broadcasted
// Syntax: mulx.xyzw vf1, vf2, vf3x (multiply vf2 by vf3.x broadcast to all lanes)
// BC field: x=0, y=1, z=2, w=3
// Opcode (bits 5-2): add=0, sub=1, madd=2, msub=3, max=4, mini=5, mul=6

let Itinerary = IIC_FMAC in {

// MUL broadcast variants (opcode = 6)
def MULx : UpperBroadcastInst<0b0110, 0b00,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "mulx$dest\t$vd, $vs, ${vt}x", []>;

def MULy : UpperBroadcastInst<0b0110, 0b01,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "muly$dest\t$vd, $vs, ${vt}y", []>;

def MULz : UpperBroadcastInst<0b0110, 0b10,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "mulz$dest\t$vd, $vs, ${vt}z", []>;

def MULw : UpperBroadcastInst<0b0110, 0b11,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "mulw$dest\t$vd, $vs, ${vt}w", []>;

// ADD broadcast variants (opcode = 0)
def ADDx : UpperBroadcastInst<0b0000, 0b00,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "addx$dest\t$vd, $vs, ${vt}x", []>;

def ADDy : UpperBroadcastInst<0b0000, 0b01,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "addy$dest\t$vd, $vs, ${vt}y", []>;

def ADDz : UpperBroadcastInst<0b0000, 0b10,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "addz$dest\t$vd, $vs, ${vt}z", []>;

def ADDw : UpperBroadcastInst<0b0000, 0b11,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "addw$dest\t$vd, $vs, ${vt}w", []>;

// SUB broadcast variants (opcode = 1)
def SUBx : UpperBroadcastInst<0b0001, 0b00,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "subx$dest\t$vd, $vs, ${vt}x", []>;

def SUBy : UpperBroadcastInst<0b0001, 0b01,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "suby$dest\t$vd, $vs, ${vt}y", []>;

def SUBz : UpperBroadcastInst<0b0001, 0b10,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "subz$dest\t$vd, $vs, ${vt}z", []>;

def SUBw : UpperBroadcastInst<0b0001, 0b11,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "subw$dest\t$vd, $vs, ${vt}w", []>;

// MADD broadcast variants (opcode = 2)
def MADDx : UpperBroadcastInst<0b0010, 0b00,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "maddx$dest\t$vd, $vs, ${vt}x", []>;

def MADDy : UpperBroadcastInst<0b0010, 0b01,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "maddy$dest\t$vd, $vs, ${vt}y", []>;

def MADDz : UpperBroadcastInst<0b0010, 0b10,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "maddz$dest\t$vd, $vs, ${vt}z", []>;

def MADDw : UpperBroadcastInst<0b0010, 0b11,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "maddw$dest\t$vd, $vs, ${vt}w", []>;

// MSUB broadcast variants (opcode = 3)
def MSUBx : UpperBroadcastInst<0b0011, 0b00,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "msubx$dest\t$vd, $vs, ${vt}x", []>;

def MSUBy : UpperBroadcastInst<0b0011, 0b01,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "msuby$dest\t$vd, $vs, ${vt}y", []>;

def MSUBz : UpperBroadcastInst<0b0011, 0b10,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "msubz$dest\t$vd, $vs, ${vt}z", []>;

def MSUBw : UpperBroadcastInst<0b0011, 0b11,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "msubw$dest\t$vd, $vs, ${vt}w", []>;

// MAX broadcast variants (opcode = 4)
def MAXx : UpperBroadcastInst<0b0100, 0b00,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "maxx$dest\t$vd, $vs, ${vt}x", []>;

def MAXy : UpperBroadcastInst<0b0100, 0b01,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "maxy$dest\t$vd, $vs, ${vt}y", []>;

def MAXz : UpperBroadcastInst<0b0100, 0b10,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "maxz$dest\t$vd, $vs, ${vt}z", []>;

def MAXw : UpperBroadcastInst<0b0100, 0b11,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "maxw$dest\t$vd, $vs, ${vt}w", []>;

// MINI broadcast variants (opcode = 5)
def MINIx : UpperBroadcastInst<0b0101, 0b00,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "minix$dest\t$vd, $vs, ${vt}x", []>;

def MINIy : UpperBroadcastInst<0b0101, 0b01,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "miniy$dest\t$vd, $vs, ${vt}y", []>;

def MINIz : UpperBroadcastInst<0b0101, 0b10,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "miniz$dest\t$vd, $vs, ${vt}z", []>;

def MINIw : UpperBroadcastInst<0b0101, 0b11,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "miniw$dest\t$vd, $vs, ${vt}w", []>;

} // Itinerary = IIC_FMAC

//===----------------------------------------------------------------------===//
// Lower Instructions
//===----------------------------------------------------------------------===//

// Integer ALU Instructions
let Itinerary = IIC_IALU in {

def IADD : LowerInst<0b0110000,
    (outs VIRegs:$id), (ins VIRegs:$is, VIRegs:$it),
    "iadd\t$id, $is, $it",
    []>;

def ISUB : LowerInst<0b0110001,
    (outs VIRegs:$id), (ins VIRegs:$is, VIRegs:$it),
    "isub\t$id, $is, $it",
    []>;

def IAND : LowerInst<0b0110100,
    (outs VIRegs:$id), (ins VIRegs:$is, VIRegs:$it),
    "iand\t$id, $is, $it",
    []>;

def IOR : LowerInst<0b0110101,
    (outs VIRegs:$id), (ins VIRegs:$is, VIRegs:$it),
    "ior\t$id, $is, $it",
    []>;

def IADDIi : LowerImmInst<0b0000110,
    (outs VIRegs:$it), (ins VIRegs:$is, uimm5:$imm),
    "iaddi\t$it, $is, $imm",
    []>;

def ISUBIi : LowerImmInst<0b0000111,
    (outs VIRegs:$it), (ins VIRegs:$is, uimm5:$imm),
    "isubi\t$it, $is, $imm",
    []>;

} // Itinerary = IIC_IALU

// Load/Store Instructions
let Itinerary = IIC_LSU in {

def LQ : LowerMemInst<0b0000000,
    (outs VFRegs:$vt), (ins destmask:$dest, VIRegs:$is, simm11:$offset),
    "lq$dest\t$vt, ${offset}(${is})",
    []>;

def SQ : LowerMemInst<0b0000001,
    (outs), (ins destmask:$dest, VFRegs:$vt, VIRegs:$is, simm11:$offset),
    "sq$dest\t$vt, ${offset}(${is})",
    []>;

def LQI : LowerMemIncInst<0b0111100,
    (outs VFRegs:$vt), (ins destmask:$dest, VIRegs:$is),
    "lqi$dest\t$vt, (${is}++)",
    []>;

def SQI : LowerMemIncInst<0b0111101,
    (outs), (ins destmask:$dest, VFRegs:$vt, VIRegs:$is),
    "sqi$dest\t$vt, (${is}++)",
    []>;

def ILW : LowerWordMemInst<0b0000100,
    (outs VIRegs:$it), (ins fieldsel:$f, VIRegs:$is, simm11:$offset),
    "ilw.$f\t$it, ${offset}(${is})",
    []>;

def ISW : LowerWordMemInst<0b0000101,
    (outs), (ins fieldsel:$f, VIRegs:$it, VIRegs:$is, simm11:$offset),
    "isw.$f\t$it, ${offset}(${is})",
    []>;

} // Itinerary = IIC_LSU

// Division/Sqrt Instructions
let Itinerary = IIC_FDIV in {

def DIV : LowerDivInst<0b0111000,
    (outs), (ins fieldsel:$fsf, VFRegs:$vs, fieldsel:$ftf, VFRegs:$vt),
    "div\tq, $vs$fsf, $vt$ftf",
    []>;

}

let Itinerary = IIC_FSQRT in {

def SQRT : LowerSqrtInst<0b0111001,
    (outs), (ins fieldsel:$ftf, VFRegs:$vt),
    "sqrt\tq, $vt$ftf",
    []>;

}

let Itinerary = IIC_FRSQRT in {

def RSQRT : LowerRsqrtInst<0b0111010,
    (outs), (ins fieldsel:$fsf, VFRegs:$vs, fieldsel:$ftf, VFRegs:$vt),
    "rsqrt\tq, $vs$fsf, $vt$ftf",
    []>;

}

// Wait Instructions
def WAITQ : LowerWaitInst<0b0111011,
    (outs), (ins),
    "waitq",
    []>;

def WAITP : LowerWaitInst<0b0111111,
    (outs), (ins),
    "waitp",
    []>;

// Branch Instructions
let Itinerary = IIC_BRU, hasDelaySlot = 1 in {

def B : LowerBranchInst<0b0100000,
    (outs), (ins simm11:$offset),
    "b\t$offset",
    []>;

def BAL : LowerBranchLinkInst<0b0100001,
    (outs VIRegs:$it), (ins simm11:$offset),
    "bal\t$it, $offset",
    []>;

def JR : LowerJumpRegInst<0b0100100,
    (outs), (ins VIRegs:$is),
    "jr\t$is",
    []>;

def JALR : LowerJumpLinkRegInst<0b0100101,
    (outs VIRegs:$it), (ins VIRegs:$is),
    "jalr\t$it, $is",
    []>;

def IBEQ : LowerCondBranchInst<0b0101000,
    (outs), (ins VIRegs:$is, VIRegs:$it, simm11:$offset),
    "ibeq\t$is, $it, $offset",
    []>;

def IBNE : LowerCondBranchInst<0b0101001,
    (outs), (ins VIRegs:$is, VIRegs:$it, simm11:$offset),
    "ibne\t$is, $it, $offset",
    []>;

def IBGTZ : LowerCondBranchSingleInst<0b0101100,
    (outs), (ins VIRegs:$is, simm11:$offset),
    "ibgtz\t$is, $offset",
    []>;

def IBGEZ : LowerCondBranchSingleInst<0b0101101,
    (outs), (ins VIRegs:$is, simm11:$offset),
    "ibgez\t$is, $offset",
    []>;

def IBLTZ : LowerCondBranchSingleInst<0b0101110,
    (outs), (ins VIRegs:$is, simm11:$offset),
    "ibltz\t$is, $offset",
    []>;

def IBLEZ : LowerCondBranchSingleInst<0b0101111,
    (outs), (ins VIRegs:$is, simm11:$offset),
    "iblez\t$is, $offset",
    []>;

} // Itinerary = IIC_BRU, hasDelaySlot = 1

// Lower NOP
def NOP_LOWER : LowerNopInst<0b0110011,
    (outs), (ins),
    "nop",
    []> {
  let Itinerary = IIC_NOP;
}

// Move Instructions
def MOVE : LowerMoveInst<0b0110011,
    (outs VFRegs:$vt), (ins destmask:$dest, VFRegs:$vs),
    "move$dest\t$vt, $vs",
    []>;

def MR32 : LowerMr32Inst<0b0110011,
    (outs VFRegs:$vt), (ins destmask:$dest, VFRegs:$vs),
    "mr32$dest\t$vt, $vs",
    []>;

def MFIR : LowerMfirInst<0b0110011,
    (outs VFRegs:$vt), (ins destmask:$dest, VIRegs:$is),
    "mfir$dest\t$vt, $is",
    []>;

def MTIR : LowerMtirInst<0b0110011,
    (outs VIRegs:$it), (ins fieldsel:$fsf, VFRegs:$vs),
    "mtir\t$it, $vs$fsf",
    []>;

//===----------------------------------------------------------------------===//
// Predicates
//===----------------------------------------------------------------------===//

def HasEFU : Predicate<"Subtarget->hasEFU()">,
             AssemblerPredicate<(all_of FeatureEFU), "EFU support">;

def IsVU1 : Predicate<"Subtarget->isVU1()">,
            AssemblerPredicate<(all_of FeatureVU1), "VU1 mode">;

//===----------------------------------------------------------------------===//
// EFU Instructions (VU1 only)
//===----------------------------------------------------------------------===//

let Itinerary = IIC_EFU, Predicates = [HasEFU] in {

// EFU instructions with unique function codes
def ESIN : LowerEfuInst<0b0111110, 0b111100,
    (outs), (ins fieldsel:$fsf, VFRegs:$vs),
    "esin\tp, $vs$fsf",
    []>;

def ECOS : LowerEfuInst<0b0111110, 0b111101,
    (outs), (ins fieldsel:$fsf, VFRegs:$vs),
    "ecos\tp, $vs$fsf",
    []>;

def EEXP : LowerEfuInst<0b0111110, 0b111110,
    (outs), (ins fieldsel:$fsf, VFRegs:$vs),
    "eexp\tp, $vs$fsf",
    []>;

def ELOG : LowerEfuInst<0b0111110, 0b111111,
    (outs), (ins fieldsel:$fsf, VFRegs:$vs),
    "elog\tp, $vs$fsf",
    []>;

def ESQRT : LowerEfuInst<0b0111110, 0b111010,
    (outs), (ins fieldsel:$fsf, VFRegs:$vs),
    "esqrt\tp, $vs$fsf",
    []>;

def ERSQRT : LowerEfuInst<0b0111110, 0b111011,
    (outs), (ins fieldsel:$fsf, VFRegs:$vs),
    "ersqrt\tp, $vs$fsf",
    []>;

def EATAN : LowerEfuInst<0b0111110, 0b111001,
    (outs), (ins fieldsel:$fsf, VFRegs:$vs),
    "eatan\tp, $vs$fsf",
    []>;

} // Itinerary = IIC_EFU, Predicates = [HasEFU]

//===----------------------------------------------------------------------===//
// Predicates
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Pseudo Instructions
//===----------------------------------------------------------------------===//

// Call frame setup/destroy pseudo instructions
let Defs = [VI15], Uses = [VI15], isCodeGenOnly = 1 in {
def ADJCALLSTACKDOWN : Pseudo<(outs), (ins i32imm:$amt1, i32imm:$amt2),
                               "# ADJCALLSTACKDOWN $amt1, $amt2",
                               []>;
def ADJCALLSTACKUP   : Pseudo<(outs), (ins i32imm:$amt1, i32imm:$amt2),
                               "# ADJCALLSTACKUP $amt1, $amt2",
                               []>;
}

//===----------------------------------------------------------------------===//
// ISel Pseudo Instructions (CodeGen only, fixed destmask=0xF for .xyzw)
//===----------------------------------------------------------------------===//
// These pseudo-instructions are used for instruction selection.
// They have no destmask operand - always use full mask (.xyzw).
// Uses UpperInstISel which has fixed dest=0xF.

// Vector Float Arithmetic (Upper pipeline) - ISel versions with full mask
// Use same opcodes as regular instructions (0x28, 0x2a, 0x2c)
def ADD_xyzw : UpperInstISel<0b101000,  // 0x28
    (outs VFRegs:$vd), (ins VFRegs:$vs, VFRegs:$vt),
    "add.xyzw\t$vd, $vs, $vt",
    [(set v4f32:$vd, (fadd v4f32:$vs, v4f32:$vt))]>;

def SUB_xyzw : UpperInstISel<0b101100,  // 0x2c
    (outs VFRegs:$vd), (ins VFRegs:$vs, VFRegs:$vt),
    "sub.xyzw\t$vd, $vs, $vt",
    [(set v4f32:$vd, (fsub v4f32:$vs, v4f32:$vt))]>;

def MUL_xyzw : UpperInstISel<0b101010,  // 0x2a
    (outs VFRegs:$vd), (ins VFRegs:$vs, VFRegs:$vt),
    "mul.xyzw\t$vd, $vs, $vt",
    [(set v4f32:$vd, (fmul v4f32:$vs, v4f32:$vt))]>;

// Integer ALU (Lower pipeline) - ISel patterns
// LowerInst already doesn't use destmask for integer ops
let isCodeGenOnly = 1 in {

def IADD_isel : LowerInst<0b0110000,
    (outs VIRegs:$id), (ins VIRegs:$is, VIRegs:$it),
    "iadd\t$id, $is, $it",
    [(set i16:$id, (add i16:$is, i16:$it))]>;

def ISUB_isel : LowerInst<0b0110001,
    (outs VIRegs:$id), (ins VIRegs:$is, VIRegs:$it),
    "isub\t$id, $is, $it",
    [(set i16:$id, (sub i16:$is, i16:$it))]>;

def IAND_isel : LowerInst<0b0110100,
    (outs VIRegs:$id), (ins VIRegs:$is, VIRegs:$it),
    "iand\t$id, $is, $it",
    [(set i16:$id, (and i16:$is, i16:$it))]>;

def IOR_isel : LowerInst<0b0110101,
    (outs VIRegs:$id), (ins VIRegs:$is, VIRegs:$it),
    "ior\t$id, $is, $it",
    [(set i16:$id, (or i16:$is, i16:$it))]>;

} // isCodeGenOnly = 1

// Load/Store ISel patterns
// Use LowerMemInstISel which has fixed dest=0xF (.xyzw)
// Simple patterns: load from register with 0 offset

// Vector Load (LQ) - 128-bit
let mayLoad = 1, isCodeGenOnly = 1 in
def LQ_xyzw : LowerMemInstISel<0b0000000,
    (outs VFRegs:$vt), (ins VIRegs:$is, simm11:$offset),
    "lq.xyzw\t$vt, ${offset}(${is})",
    []>;

// Vector Store (SQ) - 128-bit
let mayStore = 1, isCodeGenOnly = 1 in
def SQ_xyzw : LowerMemInstISel<0b0000001,
    (outs), (ins VFRegs:$vt, VIRegs:$is, simm11:$offset),
    "sq.xyzw\t$vt, ${offset}(${is})",
    []>;

// Integer Load (ILW) - 16-bit from .x field
let mayLoad = 1, isCodeGenOnly = 1 in
def ILW_x : LowerWordMemInstISel<0b0000100,
    (outs VIRegs:$it), (ins VIRegs:$is, simm11:$offset),
    "ilw.x\t$it, ${offset}(${is})",
    []>;

// Integer Store (ISW) - 16-bit to .x field
let mayStore = 1, isCodeGenOnly = 1 in
def ISW_x : LowerWordMemInstISel<0b0000101,
    (outs), (ins VIRegs:$it, VIRegs:$is, simm11:$offset),
    "isw.x\t$it, ${offset}(${is})",
    []>;

// Simple load/store patterns: load from register with 0 offset
def : Pat<(v4f32 (load VIRegs:$base)), (LQ_xyzw VIRegs:$base, 0)>;
def : Pat<(store v4f32:$src, VIRegs:$base), (SQ_xyzw v4f32:$src, VIRegs:$base, 0)>;
def : Pat<(i16 (load VIRegs:$base)), (ILW_x VIRegs:$base, 0)>;
def : Pat<(store i16:$src, VIRegs:$base), (ISW_x i16:$src, VIRegs:$base, 0)>;

//===----------------------------------------------------------------------===//
// Broadcast Multiply ISel Instructions
//===----------------------------------------------------------------------===//
// These are used for matrix-vector multiplication patterns where we need to
// multiply a vector by a scalar component broadcast.
// Example: result = matrix_row * vertex.x (broadcast x to all lanes)

// Broadcast SDNode - extracts a scalar and broadcasts it
def dvpvu_broadcast : SDNode<"DVPVUISD::BROADCAST", SDTypeProfile<1, 2,
    [SDTCisVT<0, v4f32>, SDTCisVT<1, v4f32>, SDTCisVT<2, i32>]>>;

// MUL broadcast ISel variants (fixed destmask=0xF)
def MULx_xyzw : UpperBroadcastInstISel<0b0110, 0b00,
    (outs VFRegs:$vd), (ins VFRegs:$vs, VFRegs:$vt),
    "mulx.xyzw\t$vd, $vs, ${vt}x", []>;

def MULy_xyzw : UpperBroadcastInstISel<0b0110, 0b01,
    (outs VFRegs:$vd), (ins VFRegs:$vs, VFRegs:$vt),
    "muly.xyzw\t$vd, $vs, ${vt}y", []>;

def MULz_xyzw : UpperBroadcastInstISel<0b0110, 0b10,
    (outs VFRegs:$vd), (ins VFRegs:$vs, VFRegs:$vt),
    "mulz.xyzw\t$vd, $vs, ${vt}z", []>;

def MULw_xyzw : UpperBroadcastInstISel<0b0110, 0b11,
    (outs VFRegs:$vd), (ins VFRegs:$vs, VFRegs:$vt),
    "mulw.xyzw\t$vd, $vs, ${vt}w", []>;

// ADD broadcast ISel variants (fixed destmask=0xF)
def ADDx_xyzw : UpperBroadcastInstISel<0b0000, 0b00,
    (outs VFRegs:$vd), (ins VFRegs:$vs, VFRegs:$vt),
    "addx.xyzw\t$vd, $vs, ${vt}x", []>;

def ADDy_xyzw : UpperBroadcastInstISel<0b0000, 0b01,
    (outs VFRegs:$vd), (ins VFRegs:$vs, VFRegs:$vt),
    "addy.xyzw\t$vd, $vs, ${vt}y", []>;

def ADDz_xyzw : UpperBroadcastInstISel<0b0000, 0b10,
    (outs VFRegs:$vd), (ins VFRegs:$vs, VFRegs:$vt),
    "addz.xyzw\t$vd, $vs, ${vt}z", []>;

def ADDw_xyzw : UpperBroadcastInstISel<0b0000, 0b11,
    (outs VFRegs:$vd), (ins VFRegs:$vs, VFRegs:$vt),
    "addw.xyzw\t$vd, $vs, ${vt}w", []>;

// SUB broadcast ISel variants (fixed destmask=0xF)
def SUBx_xyzw : UpperBroadcastInstISel<0b0001, 0b00,
    (outs VFRegs:$vd), (ins VFRegs:$vs, VFRegs:$vt),
    "subx.xyzw\t$vd, $vs, ${vt}x", []>;

def SUBy_xyzw : UpperBroadcastInstISel<0b0001, 0b01,
    (outs VFRegs:$vd), (ins VFRegs:$vs, VFRegs:$vt),
    "suby.xyzw\t$vd, $vs, ${vt}y", []>;

def SUBz_xyzw : UpperBroadcastInstISel<0b0001, 0b10,
    (outs VFRegs:$vd), (ins VFRegs:$vs, VFRegs:$vt),
    "subz.xyzw\t$vd, $vs, ${vt}z", []>;

def SUBw_xyzw : UpperBroadcastInstISel<0b0001, 0b11,
    (outs VFRegs:$vd), (ins VFRegs:$vs, VFRegs:$vt),
    "subw.xyzw\t$vd, $vs, ${vt}w", []>;

//===----------------------------------------------------------------------===//
// Broadcast Multiply Patterns
//===----------------------------------------------------------------------===//
// These patterns match (fmul vec, (broadcast vec, idx)) and select the
// appropriate broadcast multiply instruction.

// fmul vec, (broadcast vec2, 0) -> MULx vec, vec2
def : Pat<(fmul v4f32:$vs, (dvpvu_broadcast v4f32:$vt, (i32 0))),
          (MULx_xyzw VFRegs:$vs, VFRegs:$vt)>;
def : Pat<(fmul v4f32:$vs, (dvpvu_broadcast v4f32:$vt, (i32 1))),
          (MULy_xyzw VFRegs:$vs, VFRegs:$vt)>;
def : Pat<(fmul v4f32:$vs, (dvpvu_broadcast v4f32:$vt, (i32 2))),
          (MULz_xyzw VFRegs:$vs, VFRegs:$vt)>;
def : Pat<(fmul v4f32:$vs, (dvpvu_broadcast v4f32:$vt, (i32 3))),
          (MULw_xyzw VFRegs:$vs, VFRegs:$vt)>;

// Commutative: (broadcast vec2, idx) fmul vec -> MULx vec, vec2
def : Pat<(fmul (dvpvu_broadcast v4f32:$vt, (i32 0)), v4f32:$vs),
          (MULx_xyzw VFRegs:$vs, VFRegs:$vt)>;
def : Pat<(fmul (dvpvu_broadcast v4f32:$vt, (i32 1)), v4f32:$vs),
          (MULy_xyzw VFRegs:$vs, VFRegs:$vt)>;
def : Pat<(fmul (dvpvu_broadcast v4f32:$vt, (i32 2)), v4f32:$vs),
          (MULz_xyzw VFRegs:$vs, VFRegs:$vt)>;
def : Pat<(fmul (dvpvu_broadcast v4f32:$vt, (i32 3)), v4f32:$vs),
          (MULw_xyzw VFRegs:$vs, VFRegs:$vt)>;

