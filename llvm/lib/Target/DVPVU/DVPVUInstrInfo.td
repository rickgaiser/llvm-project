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
def FieldOperand : AsmOperandClass {
  let Name = "Field";
  let RenderMethod = "addFieldOperands";
  let ParserMethod = "parseField";
}

def field : Operand<i32> {
  let PrintMethod = "printField";
  let ParserMatchClass = FieldOperand;
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
// Upper Instructions (FMAC Pipeline)
//===----------------------------------------------------------------------===//

// ADD - Vector Add
let Itinerary = IIC_FMAC in {

def ADDvv : UpperInst<0b000000,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "add$dest\t$vd, $vs, $vt",
    [(set VFRegs:$vd, (fadd VFRegs:$vs, VFRegs:$vt))]>;

def SUBvv : UpperInst<0b000001,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "sub$dest\t$vd, $vs, $vt",
    [(set VFRegs:$vd, (fsub VFRegs:$vs, VFRegs:$vt))]>;

def MULvv : UpperInst<0b011100,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "mul$dest\t$vd, $vs, $vt",
    [(set VFRegs:$vd, (fmul VFRegs:$vs, VFRegs:$vt))]>;

def MADDvv : UpperInst<0b000010,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "madd$dest\t$vd, $vs, $vt",
    []>;  // Uses ACC

def MSUBvv : UpperInst<0b000011,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "msub$dest\t$vd, $vs, $vt",
    []>;

def MAXvv : UpperInst<0b011011,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "max$dest\t$vd, $vs, $vt",
    []>;

def MINIvv : UpperInst<0b011111,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs, VFRegs:$vt),
    "mini$dest\t$vd, $vs, $vt",
    []>;

def ABSv : UpperInst<0b111101,
    (outs VFRegs:$vd), (ins destmask:$dest, VFRegs:$vs),
    "abs$dest\t$vd, $vs",
    [(set VFRegs:$vd, (fabs VFRegs:$vs))]>;

// Upper NOP
def NOP_UPPER : UpperInst<0b101111,
    (outs), (ins),
    "nop",
    []> {
  let Itinerary = IIC_NOP;
}

} // Itinerary = IIC_FMAC

//===----------------------------------------------------------------------===//
// Lower Instructions
//===----------------------------------------------------------------------===//

// Integer ALU Instructions
let Itinerary = IIC_IALU in {

def IADD : LowerInst<0b0110000,
    (outs VIRegs:$id), (ins VIRegs:$is, VIRegs:$it),
    "iadd\t$id, $is, $it",
    [(set VIRegs:$id, (add VIRegs:$is, VIRegs:$it))]>;

def ISUB : LowerInst<0b0110001,
    (outs VIRegs:$id), (ins VIRegs:$is, VIRegs:$it),
    "isub\t$id, $is, $it",
    [(set VIRegs:$id, (sub VIRegs:$is, VIRegs:$it))]>;

def IAND : LowerInst<0b0110100,
    (outs VIRegs:$id), (ins VIRegs:$is, VIRegs:$it),
    "iand\t$id, $is, $it",
    [(set VIRegs:$id, (and VIRegs:$is, VIRegs:$it))]>;

def IOR : LowerInst<0b0110101,
    (outs VIRegs:$id), (ins VIRegs:$is, VIRegs:$it),
    "ior\t$id, $is, $it",
    [(set VIRegs:$id, (or VIRegs:$is, VIRegs:$it))]>;

def IADDIi : LowerImmInst<0b0000110,
    (outs VIRegs:$it), (ins VIRegs:$is, uimm5:$imm),
    "iaddi\t$it, $is, $imm",
    [(set VIRegs:$it, (add VIRegs:$is, uimm5:$imm))]>;

def ISUBIi : LowerImmInst<0b0000111,
    (outs VIRegs:$it), (ins VIRegs:$is, uimm5:$imm),
    "isubi\t$it, $is, $imm",
    [(set VIRegs:$it, (sub VIRegs:$is, uimm5:$imm))]>;

} // Itinerary = IIC_IALU

// Load/Store Instructions
let Itinerary = IIC_LSU in {

def LQ : LowerMemInst<0b0000000,
    (outs VFRegs:$vt), (ins destmask:$dest, memvi:$addr),
    "lq$dest\t$vt, $addr",
    []>;

def SQ : LowerMemInst<0b0000001,
    (outs), (ins destmask:$dest, VFRegs:$vs, memvi:$addr),
    "sq$dest\t$vs, $addr",
    []>;

def LQI : LowerMemIncInst<0b0111100,
    (outs VFRegs:$vt), (ins destmask:$dest, VIRegs:$is),
    "lqi$dest\t$vt, ($is++)",
    []>;

def SQI : LowerMemIncInst<0b0111101,
    (outs), (ins destmask:$dest, VFRegs:$vs, VIRegs:$is),
    "sqi$dest\t$vs, ($is++)",
    []>;

def ILW : LowerWordMemInst<0b0000100,
    (outs VIRegs:$it), (ins field:$f, memvi:$addr),
    "ilw.$f\t$it, $addr",
    []>;

def ISW : LowerWordMemInst<0b0000101,
    (outs), (ins field:$f, VIRegs:$is, memvi:$addr),
    "isw.$f\t$is, $addr",
    []>;

} // Itinerary = IIC_LSU

// Division/Sqrt Instructions
let Itinerary = IIC_FDIV in {

def DIV : LowerDivInst<0b0111000,
    (outs), (ins field:$fsf, VFRegs:$vs, field:$ftf, VFRegs:$vt),
    "div\tq, $vs$fsf, $vt$ftf",
    []>;

}

let Itinerary = IIC_FSQRT in {

def SQRT : LowerSqrtInst<0b0111001,
    (outs), (ins field:$ftf, VFRegs:$vt),
    "sqrt\tq, $vt$ftf",
    []>;

}

let Itinerary = IIC_FRSQRT in {

def RSQRT : LowerRsqrtInst<0b0111010,
    (outs), (ins field:$fsf, VFRegs:$vs, field:$ftf, VFRegs:$vt),
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
    [(br bb:$offset)]>;

def BAL : LowerBranchLinkInst<0b0100001,
    (outs VIRegs:$it), (ins simm11:$offset),
    "bal\t$it, $offset",
    []>;

def JR : LowerJumpRegInst<0b0100100,
    (outs), (ins VIRegs:$is),
    "jr\t$is",
    [(brind VIRegs:$is)]>;

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
    (outs VIRegs:$it), (ins field:$fsf, VFRegs:$vs),
    "mtir\t$it, $vs$fsf",
    []>;

//===----------------------------------------------------------------------===//
// EFU Instructions (VU1 only)
//===----------------------------------------------------------------------===//

let Itinerary = IIC_EFU, Predicates = [HasEFU] in {

def ESIN : LowerEfuInst<0b0111110,
    (outs), (ins field:$fsf, VFRegs:$vs),
    "esin\tp, $vs$fsf",
    []>;

def ECOS : LowerEfuInst<0b0111110,
    (outs), (ins field:$fsf, VFRegs:$vs),
    "ecos\tp, $vs$fsf",
    []>;

def EEXP : LowerEfuInst<0b0111110,
    (outs), (ins field:$fsf, VFRegs:$vs),
    "eexp\tp, $vs$fsf",
    []>;

def ELOG : LowerEfuInst<0b0111110,
    (outs), (ins field:$fsf, VFRegs:$vs),
    "elog\tp, $vs$fsf",
    []>;

def ESQRT : LowerEfuInst<0b0111110,
    (outs), (ins field:$fsf, VFRegs:$vs),
    "esqrt\tp, $vs$fsf",
    []>;

def ERSQRT : LowerEfuInst<0b0111110,
    (outs), (ins field:$fsf, VFRegs:$vs),
    "ersqrt\tp, $vs$fsf",
    []>;

def EATAN : LowerEfuInst<0b0111110,
    (outs), (ins field:$fsf, VFRegs:$vs),
    "eatan\tp, $vs$fsf",
    []>;

} // Itinerary = IIC_EFU, Predicates = [HasEFU]

//===----------------------------------------------------------------------===//
// Predicates
//===----------------------------------------------------------------------===//

def HasEFU : Predicate<"Subtarget->hasEFU()">,
             AssemblerPredicate<(all_of FeatureEFU), "EFU support">;

def IsVU1 : Predicate<"Subtarget->isVU1()">,
            AssemblerPredicate<(all_of FeatureVU1), "VU1 mode">;
