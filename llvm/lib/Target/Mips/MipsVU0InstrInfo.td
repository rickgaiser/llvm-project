//===- MipsVU0InstrInfo.td - VU0 Macro Mode Instructions ----*- tablegen
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file describes R5900 VU0 (COP2) macro mode instructions.
// Requires: MipsVU0InstrFormats.td
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// VU0 Quadword Load/Store
//===----------------------------------------------------------------------===//
// VU0 uses 128-bit VF registers for SIMD operations.
// LQC2/SQC2 use the same opcodes as LDC2/SDC2 (0x36/0x3E) but transfer 128-bit.

class LW_VF<string opstr, RegisterOperand RC,
            SDPatternOperator OpNode = null_frag>
    : InstSE<(outs RC:$rt), (ins mem_simm16:$addr),
             !strconcat(opstr, "\t$rt, $addr"),
             [(set RC:$rt, (OpNode addr:$addr))], FrmFI, opstr> {
  let DecoderMethod = "DecodeVFMem";
  let DecoderNamespace = "R5900";
  let mayLoad = 1;
}

class SW_VF<string opstr, RegisterOperand RC,
            SDPatternOperator OpNode = null_frag>
    : InstSE<(outs), (ins RC:$rt, mem_simm16:$addr),
             !strconcat(opstr, "\t$rt, $addr"),
             [(OpNode RC:$rt, addr:$addr)], FrmFI, opstr> {
  let DecoderMethod = "DecodeVFMem";
  let DecoderNamespace = "R5900";
  let mayStore = 1;
}

let Predicates = [HasVU0] in {
  // LQC2 - Load Quadword to COP2 (128-bit memory -> VF register)
  def LQC2 : LW_VF<"lqc2", VFOpnd, load>, LW_FM<0x36>;

  // SQC2 - Store Quadword from COP2 (VF register -> 128-bit memory)
  def SQC2 : SW_VF<"sqc2", VFOpnd, store>, LW_FM<0x3e>;
} // Predicates = [HasVU0]

//===----------------------------------------------------------------------===//
// VU0 (COP2) Arithmetic Instructions - R5900
//===----------------------------------------------------------------------===//
// VU0 macro mode arithmetic instructions use the COP2 encoding with
// 3 VF registers and a destination mask (xyzw).

//===----------------------------------------------------------------------===//
// VU0 Destination Mask Operand
//===----------------------------------------------------------------------===//
// The 4-bit destination mask controls which components (x,y,z,w) are written.
// Bit 3 = x, Bit 2 = y, Bit 1 = z, Bit 0 = w

def VU0DestMaskAsmOperand : AsmOperandClass {
  let Name = "VU0DestMask";
  let RenderMethod = "addImmOperands";
  let ParserMethod = "parseVU0DestMask";
}

def vu0destmask : Operand<i32> {
  let PrintMethod = "printVU0DestMask";
  let ParserMatchClass = VU0DestMaskAsmOperand;
}

// VU0 ACC operand - for instructions that explicitly specify ACC as destination
// The ACC is always implicit in the encoding, but assembly may specify it
// explicitly
def VU0AccAsmOperand : AsmOperandClass {
  let Name = "VU0Acc";
  let RenderMethod = "addVU0AccAsmRegOperands";
  let ParserMethod = "parseVU0Acc";
}

def VU0AccOpnd : RegisterOperand<VU0ACCRC> {
  let ParserMatchClass = VU0AccAsmOperand;
}

// VU0 Q register operand - for instructions that use the Q result register
// Used by VDIV, VSQRT, VRSQRT (write to Q) and VMULQ, etc. (read from Q)
def VU0QAsmOperand : AsmOperandClass {
  let Name = "VU0Q";
  let RenderMethod = "addVU0QAsmRegOperands";
  let ParserMethod = "parseVU0Q";
}

def VU0QOpnd : RegisterOperand<VU0QRC> {
  let ParserMatchClass = VU0QAsmOperand;
  // Q is not encoded.
  let DecoderMethod = "DecodeVU0Q";
}

// VU0 Field selector operand - for instructions like VDIV, VRSQRT, VSQRT
// that select a single component (x/y/z/w) from a VF register
// Encoding: 00=x, 01=y, 10=z, 11=w
def VU0FieldSelAsmOperand : AsmOperandClass {
  let Name = "VU0FieldSel";
  let RenderMethod = "addImmOperands";
  let ParserMethod = "parseVU0FieldSel";
}

def vu0fieldsel : Operand<i32> {
  let PrintMethod = "printVU0FieldSel";
  let ParserMatchClass = VU0FieldSelAsmOperand;
}

// VF register with field selector - combines VF register and field in one
// operand Used for instructions like VDIV/VRSQRT that use syntax: $vf0w The
// parser extracts both the register and field from the combined syntax This
// creates TWO MC operands: field selector (2-bit immediate) + VF register
def VFWithFieldAsmOperand : AsmOperandClass {
  let Name = "VFWithField";
  let RenderMethod = "addVFWithFieldOperands";
  let ParserMethod = "parseVFWithField";
}

// A field of a VF register: a VF register and a field selector (0=x, 1=y,
// 2=z, 3=w), encoded as 7 bits: the field in bits 6-5 and the register in
// bits 4-0.
def VFWithFieldOpnd : Operand<i32> {
  let ParserMatchClass = VFWithFieldAsmOperand;
  let PrintMethod = "printVFWithField";
  let EncoderMethod = "getVFWithFieldEncoding";
  let DecoderMethod = "DecodeVFWithField";
  let MIOperandInfo = (ops VFRegs, i32imm);
}

//===----------------------------------------------------------------------===//
// VU0 SDNodes
//===----------------------------------------------------------------------===//

// SDTypeProfile for VU0 masked operations: (result, base, other, mask)
// result[i] = (mask[i] ? op(base[i], other[i]) : base[i])
def SDT_VU0Masked : SDTypeProfile<1, 3,
                                  [SDTCisVT<0, v4f32>, SDTCisVT<1, v4f32>,
                                   SDTCisVT<2, v4f32>, SDTCisVT<3, i32>]>;

// SDNodes for VU0 masked operations (blend folded into arithmetic)
def MipsVU0BLEND : SDNode<"MipsISD::VU0_BLEND", SDT_VU0Masked>;
def MipsVU0FADD_MASKED : SDNode<"MipsISD::VU0_FADD_MASKED", SDT_VU0Masked>;
def MipsVU0FSUB_MASKED : SDNode<"MipsISD::VU0_FSUB_MASKED", SDT_VU0Masked>;
def MipsVU0FMUL_MASKED : SDNode<"MipsISD::VU0_FMUL_MASKED", SDT_VU0Masked>;

// Fixed-point conversions with n fraction bits (4, 12 or 15):
// VU0_FTOI = fp_to_sint (v * 2^n), VU0_ITOF = sint_to_fp (v) * 2^-n.
def MipsVU0FTOI : SDNode<"MipsISD::VU0_FTOI",
                         SDTypeProfile<1, 2, [SDTCisVT<0, v4i32>,
                                              SDTCisVT<1, v4f32>,
                                              SDTCisVT<2, i32>]>>;
def MipsVU0ITOF : SDNode<"MipsISD::VU0_ITOF",
                         SDTypeProfile<1, 2, [SDTCisVT<0, v4f32>,
                                              SDTCisVT<1, v4i32>,
                                              SDTCisVT<2, i32>]>>;

// A sum of products computed through ACC: VMULA, VMADDA/VMSUBA...,
// VMADD/VMSUB. The operands are (a, b, field, negate) for each term, with
// field 0-3 for a broadcast of b and 4 for all fields; see
// performVU0AccSumCombine. Selected in MipsSEDAGToDAGISel.
def MipsVU0ACCSUM : SDNode<"MipsISD::VU0_ACCSUM",
                           SDTypeProfile<1, -1, [SDTCisVT<0, v4f32>]>>;

// Broadcast field $f (x = 0 ... w = 3) of a vector to all four fields. It is
// selected as the ft operand of a broadcast instruction (fs op ft.<field>).
def MipsVU0BCAST : SDNode<"MipsISD::VU0_BCAST",
                          SDTypeProfile<1, 2, [SDTCisVT<0, v4f32>,
                                               SDTCisVT<1, v4f32>,
                                               SDTCisVT<2, i32>]>>;

//===----------------------------------------------------------------------===//
// VU0 Instruction Classes
//===----------------------------------------------------------------------===//
//
// Instruction classes:
// - VU0_Type1: 3 VF registers + 6-bit opcode (VADD, VSUB, VMUL, VMADD, VMSUB)
// - VU0_Type0: 3 VF registers + 4-bit opcode + 2-bit bc (VADDbc, VSUBbc,
// VMULbc)
// - VU0_Type3: 2 VF registers + 11-bit opcode (VADDA, VSUBA, VMULA - ACC dest)
// - VU0_Type2: 2 VF registers + 9-bit opcode + 2-bit bc (VADDAbc, etc.)

// VU0 Type 1 instruction class: 3 VF registers, 6-bit opcode, variable dest
// mask Used for VADD, VSUB, VMUL, VMADD, VMSUB - assembly visible with variable
// mask
class VU0_3R<string opstr, bits<6> funct>
    : InstSE<(outs VFOpnd:$fd), (ins vu0destmask:$dest, VFOpnd:$fs, VFOpnd:$ft),
             !strconcat(opstr, "$dest\t$fd, $fs, $ft"), [], FrmR, opstr> {
  let DecoderNamespace = "R5900";
  let hasSideEffects = 0;
  bits<5> fd;
  bits<5> fs;
  bits<5> ft;
  bits<4> dest;
  bits<32> Inst;
  let Inst{31 -26} = 0b010010; // COP2
  let Inst{25} = 1;            // co bit
  let Inst{24 -21} = dest;     // Variable destination mask from operand
  let Inst{20 -16} = ft;
  let Inst{15 -11} = fs;
  let Inst{10 -6} = fd;
  let Inst{5 -0} = funct;
}

// VU0 Type 3 instruction class: 2 VF registers, ACC destination, 11-bit opcode
// Used for VADDA, VSUBA, VMULA, VMADDA, VMSUBA - assembly visible with variable
// mask ACC operand is explicit in assembly but implicit in encoding (always
// ACC)
class VU0_ACC<string opstr, bits<11> funct, list<Register> UseRegs = []>
    : InstSE<(outs),
             (ins vu0destmask:$dest, VU0AccOpnd:$accd, VFOpnd:$fs, VFOpnd:$ft),
             !strconcat(opstr, "$dest\t$accd, $fs, $ft"), [], FrmR,
             opstr> {
  let DecoderNamespace = "R5900";
  let Defs = [VU0ACC];
  let Uses = UseRegs;
  let hasSideEffects = 0;
  // The ACC operand of the first instruction of a chain is not read
  // (MipsTargetLowering::AdjustInstrPostInstrSelection).
  let hasPostISelHook = !empty(UseRegs);
  bits<5> fs;
  bits<5> ft;
  bits<4> dest;
  bits<32> Inst;
  let Inst{31 -26} = 0b010010; // COP2
  let Inst{25} = 1;            // co bit
  let Inst{24 -21} = dest;     // Variable destination mask from operand
  let Inst{20 -16} = ft;
  let Inst{15 -11} = fs;
  let Inst{10 -0} = funct;
}

// VU0 Type 1 with ACC read: 3 VF registers, uses ACC, 6-bit opcode
// Used for VMADD, VMSUB (reads ACC, writes VF) - assembly visible with variable
// mask
class VU0_3R_ACC<string opstr, bits<6> funct>
    : InstSE<(outs VFOpnd:$fd), (ins vu0destmask:$dest, VFOpnd:$fs, VFOpnd:$ft),
             !strconcat(opstr, "$dest\t$fd, $fs, $ft"), [], FrmR, opstr> {
  let DecoderNamespace = "R5900";
  let Uses = [VU0ACC];
  let hasSideEffects = 0;
  bits<5> fd;
  bits<5> fs;
  bits<5> ft;
  bits<4> dest;
  bits<32> Inst;
  let Inst{31 -26} = 0b010010; // COP2
  let Inst{25} = 1;            // co bit
  let Inst{24 -21} = dest;     // Variable destination mask from operand
  let Inst{20 -16} = ft;
  let Inst{15 -11} = fs;
  let Inst{10 -6} = fd;
  let Inst{5 -0} = funct;
}

// VU0 Type 1 with dynamic destination mask: 3 VF registers, variable mask
// Used for masked VADD, VSUB, VMUL (blend operations)
class VU0_3R_Masked<string opstr, bits<6> funct>
    : InstSE<(outs VFOpnd:$fd),
             (ins VFOpnd:$base, VFOpnd:$fs, VFOpnd:$ft, vu0destmask:$mask),
             !strconcat(opstr, "$mask\t$fd, $fs, $ft"), [], FrmR, opstr> {
  let DecoderNamespace = "R5900";
  let hasSideEffects = 0;
  let isCodeGenOnly = 1; // Not for assembly, only for ISel
  bits<5> fd;
  bits<5> fs;
  bits<5> ft;
  bits<4> mask;
  bits<32> Inst;
  let Inst{31 -26} = 0b010010; // COP2
  let Inst{25} = 1;            // co bit
  let Inst{24 -21} = mask;     // Variable destination mask from operand
  let Inst{20 -16} = ft;
  let Inst{15 -11} = fs;
  let Inst{10 -6} = fd;
  let Inst{5 -0} = funct;
  let Constraints = "$base = $fd";
}

// VU0 VMOVE instruction class: 2 VF registers, masked move
// VMOVE.dest ft, fs - copies selected lanes from fs to ft
// Note: ft is the destination (not fd like in arithmetic ops)
// Encoding: Type 3 format with 11-bit opcode = 0x33C
class VU0_MOVE_Masked<string opstr, bits<11> funct>
    : InstSE<(outs VFOpnd:$ft),
             (ins VFOpnd:$base, VFOpnd:$fs, vu0destmask:$mask),
             !strconcat(opstr, "$mask\t$ft, $fs"), [], FrmR, opstr> {
  let DecoderNamespace = "R5900";
  let hasSideEffects = 0;
  let isCodeGenOnly = 1; // Not for assembly, only for ISel
  bits<5> ft;
  bits<5> fs;
  bits<4> mask;
  bits<32> Inst;
  let Inst{31 -26} = 0b010010; // COP2
  let Inst{25} = 1;            // co bit
  let Inst{24 -21} = mask;     // Variable destination mask from operand
  let Inst{20 -16} = ft;
  let Inst{15 -11} = fs;
  let Inst{10 -0} = funct;
  let Constraints = "$base = $ft";
}

// VU0 Type 3 instruction class: 2 VF registers (ft=dest, fs=src), 11-bit opcode
// Used for VABS, VMOVE, etc. - variable dest mask
class VU0_2R<string opstr, bits<11> funct>
    : InstSE<(outs VFOpnd:$ft), (ins vu0destmask:$dest, VFOpnd:$fs),
             !strconcat(opstr, "$dest\t$ft, $fs"), [], FrmR, opstr> {
  let DecoderNamespace = "R5900";
  let hasSideEffects = 0;
  bits<5> ft;
  bits<5> fs;
  bits<4> dest;
  bits<32> Inst;
  let Inst{31 -26} = 0b010010; // COP2
  let Inst{25} = 1;            // co bit
  let Inst{24 -21} = dest;     // Variable destination mask from operand
  let Inst{20 -16} = ft;
  let Inst{15 -11} = fs;
  let Inst{10 -0} = funct;
}

let Predicates = [HasVU0] in {
  // VU0 Vector Add/Subtract/Multiply (3-register forms, variable dest mask)
  // Assembly-visible instructions with variable mask operand
  let isCommutable = 1 in {
    def VADD : VU0_3R<"vadd", 0b101000>; // 0x28
    def VMUL : VU0_3R<"vmul", 0b101010>; // 0x2A
  }
  def VSUB : VU0_3R<"vsub", 0b101100>; // 0x2C

  // VU0 Vector Min/Max (3-register forms)
  let isCommutable = 1 in {
    def VMAX : VU0_3R<"vmax", 0b101011>;    // 0x2B
    def VMINI : VU0_3R<"vmini", 0b101111>; // 0x2F
  }

  // VU0 Absolute value (2-register form: ft = |fs|)
  def VABS : VU0_2R<"vabs", 0b00111111101>; // 0x1FD

  // VU0 VMOVE (move) - opcode 0x33C
  // VMOVE.dest ft, fs - copies selected lanes from fs to ft
  def VMOVE : VU0_2R<"vmove", 0b01100111100>; // 0x33C

  // VU0 VMR32 (move rotate 32) - opcode 0x33D
  // VMR32.dest ft, fs - rotates components: ft.x=fs.y, ft.y=fs.z, ft.z=fs.w,
  // ft.w=fs.x
  def VMR32 : VU0_2R<"vmr32", 0b01100111101>; // 0x33D

  // VU0 conversions between float and fixed point with 0, 4, 12 or 15
  // fraction bits. VFTOI rounds toward zero.
  def VFTOI0 : VU0_2R<"vftoi0", 0b00101111100>;   // 0x17C
  def VFTOI4 : VU0_2R<"vftoi4", 0b00101111101>;   // 0x17D
  def VFTOI12 : VU0_2R<"vftoi12", 0b00101111110>; // 0x17E
  def VFTOI15 : VU0_2R<"vftoi15", 0b00101111111>; // 0x17F
  def VITOF0 : VU0_2R<"vitof0", 0b00100111100>;   // 0x13C
  def VITOF4 : VU0_2R<"vitof4", 0b00100111101>;   // 0x13D
  def VITOF12 : VU0_2R<"vitof12", 0b00100111110>; // 0x13E
  def VITOF15 : VU0_2R<"vitof15", 0b00100111111>; // 0x13F

  // VU0 Multiply-Accumulate (uses ACC register)
  def VMADD : VU0_3R_ACC<"vmadd", 0b101001>; // 0x29
  def VMSUB : VU0_3R_ACC<"vmsub", 0b101101>; // 0x2D

  // VU0 ACC-destination instructions (writes to ACC)
  def VADDA
      : VU0_ACC<"vadda", 0b01010111100>; // bits 10-0 = 0x2BC
  def VSUBA
      : VU0_ACC<"vsuba", 0b01011111100>; // bits 10-0 = 0x2FC
  def VMULA
      : VU0_ACC<"vmula", 0b01010111110>; // bits 10-0 = 0x2BE

  // VU0 Multiply-Add/Sub to ACC (reads and writes ACC)
  def VMADDA
      : VU0_ACC<"vmadda", 0b01010111101, [VU0ACC]>; // 0x2BD
  def VMSUBA
      : VU0_ACC<"vmsuba", 0b01011111101, [VU0ACC]>; // 0x2FD

  //===----------------------------------------------------------------------===//
  // VU0 Masked Instructions (dynamic destination mask)
  //===----------------------------------------------------------------------===//
  // These instructions have a variable destination mask for blend operations.
  // The mask is an immediate operand that selects which components to write.

  def VADDm : VU0_3R_Masked<"vadd", 0b101000>; // 0x28
  def VSUBm : VU0_3R_Masked<"vsub", 0b101100>; // 0x2C
  def VMULm : VU0_3R_Masked<"vmul", 0b101010>; // 0x2A

  // VU0 VMOVE (masked move) - opcode 0x33C
  // VMOVE.dest ft, fs - copies selected lanes from fs to ft
  def VMOVEm : VU0_MOVE_Masked<"vmove", 0b01100111100>; // 0x33C

  // Patterns for masked operations
  // Match: (VU0_FADD_MASKED base, other, mask) -> VADDm(base, base, other,
  // mask)
  def : Pat<(MipsVU0FADD_MASKED v4f32:$base, v4f32:$other, (i32 imm:$mask)),
            (VADDm VFRegs:$base, VFRegs:$base, VFRegs:$other, (i32 imm:$mask))>;
  def : Pat<(MipsVU0FSUB_MASKED v4f32:$base, v4f32:$other, (i32 imm:$mask)),
            (VSUBm VFRegs:$base, VFRegs:$base, VFRegs:$other, (i32 imm:$mask))>;
  def : Pat<(MipsVU0FMUL_MASKED v4f32:$base, v4f32:$other, (i32 imm:$mask)),
            (VMULm VFRegs:$base, VFRegs:$base, VFRegs:$other, (i32 imm:$mask))>;

  // Match: (VU0_BLEND base, src, mask) -> VMOVEm(base, src, mask)
  // VU0_BLEND(base, src, mask) means: result[i] = mask[i] ? src[i] : base[i]
  def : Pat<(MipsVU0BLEND v4f32:$base, v4f32:$src, (i32 imm:$mask)),
            (VMOVEm VFRegs:$base, VFRegs:$src, (i32 imm:$mask))>;

  // Operations on all four fields: the xyzw destination mask.
  // Note: VU0 min/max don't follow IEEE NaN semantics, use with -ffast-math
  def : Pat<(fadd v4f32:$fs, v4f32:$ft), (VADD 15, v4f32:$fs, v4f32:$ft)>;
  def : Pat<(fsub v4f32:$fs, v4f32:$ft), (VSUB 15, v4f32:$fs, v4f32:$ft)>;
  def : Pat<(fmul v4f32:$fs, v4f32:$ft), (VMUL 15, v4f32:$fs, v4f32:$ft)>;
  def : Pat<(fmaxnum v4f32:$fs, v4f32:$ft), (VMAX 15, v4f32:$fs, v4f32:$ft)>;
  def : Pat<(fminnum v4f32:$fs, v4f32:$ft), (VMINI 15, v4f32:$fs, v4f32:$ft)>;
  def : Pat<(fabs v4f32:$fs), (VABS 15, v4f32:$fs)>;

} // Predicates = [HasVU0]

//===----------------------------------------------------------------------===//
// VU0 Broadcast Instructions - R5900
//===----------------------------------------------------------------------===//
// Broadcast instructions multiply/add/sub each lane by a single broadcast
// component (x, y, z, or w) from the ft register. Essential for matrix-vector
// multiplication.
//
// MacroOP field type 0: 3 registers + 4-bit opcode + 2-bit bc
// MacroOP field type 2: 2 registers (ACC dest) + 9-bit opcode + 2-bit bc

// VU0 Type 0 instruction class: 3 VF registers + 4-bit opcode + 2-bit bc
// Used for VADDbc, VSUBbc, VMULbc - assembly visible with variable dest mask
// Mnemonic includes broadcast suffix (e.g., vaddx, vmuly), dest mask is operand
class VU0_3R_BC<string opstr, string suffix, bits<4> opcode, bits<2> bc>
    : InstSE<(outs VFOpnd:$fd), (ins vu0destmask:$dest, VFOpnd:$fs, VFOpnd:$ft),
             !strconcat(opstr, suffix, "$dest\t$fd, $fs, $ft"), [], FrmR,
             opstr> {
  let DecoderNamespace = "R5900";
  let hasSideEffects = 0;
  bits<5> fd;
  bits<5> fs;
  bits<5> ft;
  bits<4> dest;
  bits<32> Inst;
  let Inst{31 -26} = 0b010010; // COP2
  let Inst{25} = 1;            // co bit
  let Inst{24 -21} = dest;     // Variable destination mask from operand
  let Inst{20 -16} = ft;
  let Inst{15 -11} = fs;
  let Inst{10 -6} = fd;
  let Inst{5 -2} = opcode;
  let Inst{1 -0} = bc;
}

// VU0 Type 0 with ACC read: 3 VF registers + 4-bit opcode + 2-bit bc
// Used for VMADDbc, VMSUBbc (reads ACC, writes VF) - variable dest mask
class VU0_3R_BC_ACC<string opstr, string suffix, bits<4> opcode, bits<2> bc>
    : InstSE<(outs VFOpnd:$fd), (ins vu0destmask:$dest, VFOpnd:$fs, VFOpnd:$ft),
             !strconcat(opstr, suffix, "$dest\t$fd, $fs, $ft"), [], FrmR,
             opstr> {
  let DecoderNamespace = "R5900";
  let Uses = [VU0ACC];
  let hasSideEffects = 0;
  bits<5> fd;
  bits<5> fs;
  bits<5> ft;
  bits<4> dest;
  bits<32> Inst;
  let Inst{31 -26} = 0b010010; // COP2
  let Inst{25} = 1;            // co bit
  let Inst{24 -21} = dest;     // Variable destination mask from operand
  let Inst{20 -16} = ft;
  let Inst{15 -11} = fs;
  let Inst{10 -6} = fd;
  let Inst{5 -2} = opcode;
  let Inst{1 -0} = bc;
}

// VU0 Type 2 instruction class: 2 VF registers, ACC dest, 9-bit opcode + 2-bit
// bc Used for VADDAbc, VSUBAbc, VMULAbc, VMADDAbc, VMSUBAbc - variable dest
// mask ACC operand is explicit in assembly but implicit in encoding
class VU0_ACC_BC<string opstr, string suffix, bits<9> opcode, bits<2> bc,
                 list<Register> UseRegs = []>
    : InstSE<(outs),
             (ins vu0destmask:$dest, VU0AccOpnd:$accd, VFOpnd:$fs, VFOpnd:$ft),
             !strconcat(opstr, suffix, "$dest\t$accd, $fs, $ft"), [], FrmR,
             opstr> {
  let DecoderNamespace = "R5900";
  let Defs = [VU0ACC];
  let Uses = UseRegs;
  let hasSideEffects = 0;
  // The ACC operand of the first instruction of a chain is not read
  // (MipsTargetLowering::AdjustInstrPostInstrSelection).
  let hasPostISelHook = !empty(UseRegs);
  bits<5> fs;
  bits<5> ft;
  bits<4> dest;
  bits<32> Inst;
  let Inst{31 -26} = 0b010010; // COP2
  let Inst{25} = 1;            // co bit
  let Inst{24 -21} = dest;     // Variable destination mask from operand
  let Inst{20 -16} = ft;
  let Inst{15 -11} = fs;
  let Inst{10 -2} = opcode;
  let Inst{1 -0} = bc;
}

// Multiclass to define all 4 broadcast variants (x, y, z, w) for 3-register ops
multiclass VU0_3R_BC_ALL<string opstr, bits<4> opcode> {
  def x : VU0_3R_BC<opstr, "x", opcode, 0b00>;
  def y : VU0_3R_BC<opstr, "y", opcode, 0b01>;
  def z : VU0_3R_BC<opstr, "z", opcode, 0b10>;
  def w : VU0_3R_BC<opstr, "w", opcode, 0b11>;
}

// Multiclass for 3-register ops that read ACC
multiclass VU0_3R_BC_ACC_ALL<string opstr, bits<4> opcode> {
  def x : VU0_3R_BC_ACC<opstr, "x", opcode, 0b00>;
  def y : VU0_3R_BC_ACC<opstr, "y", opcode, 0b01>;
  def z : VU0_3R_BC_ACC<opstr, "z", opcode, 0b10>;
  def w : VU0_3R_BC_ACC<opstr, "w", opcode, 0b11>;
}

// Multiclass for ACC-destination broadcast ops
multiclass VU0_ACC_BC_ALL<string opstr, bits<9> opcode,
                          list<Register> UseRegs = []> {
  def x : VU0_ACC_BC<opstr, "x", opcode, 0b00, UseRegs>;
  def y : VU0_ACC_BC<opstr, "y", opcode, 0b01, UseRegs>;
  def z : VU0_ACC_BC<opstr, "z", opcode, 0b10, UseRegs>;
  def w : VU0_ACC_BC<opstr, "w", opcode, 0b11, UseRegs>;
}

let Predicates = [HasVU0] in {
  // VU0 3-register broadcast instructions (Type 0)
  // VADDbc: fd = fs + ft[bc] (opcode = 0000)
  defm VADDbc : VU0_3R_BC_ALL<"vadd", 0b0000>;

  // VSUBbc: fd = fs - ft[bc] (opcode = 0001)
  defm VSUBbc : VU0_3R_BC_ALL<"vsub", 0b0001>;

  // VMAXbc: fd = max(fs, ft[bc]) (opcode = 0100)
  defm VMAXbc : VU0_3R_BC_ALL<"vmax", 0b0100>;

  // VMINIbc: fd = min(fs, ft[bc]) (opcode = 0101)
  defm VMINIbc : VU0_3R_BC_ALL<"vmini", 0b0101>;

  // VMULbc: fd = fs * ft[bc] (opcode = 0110)
  defm VMULbc : VU0_3R_BC_ALL<"vmul", 0b0110>;

  // VMADDbc: fd = ACC + fs * ft[bc] (opcode = 0010)
  defm VMADDbc : VU0_3R_BC_ACC_ALL<"vmadd", 0b0010>;

  // VMSUBbc: fd = ACC - fs * ft[bc] (opcode = 0011)
  defm VMSUBbc : VU0_3R_BC_ACC_ALL<"vmsub", 0b0011>;

  // VU0 ACC-destination broadcast instructions (Type 2)
  // Opcode format: upper 5 bits + 1111
  // VADDAbc: ACC = fs + ft[bc] (opcode = 00000 1111 = 0x0F)
  defm VADDAbc : VU0_ACC_BC_ALL<"vadda", 0b000001111>;

  // VSUBAbc: ACC = fs - ft[bc] (opcode = 00001 1111 = 0x1F)
  defm VSUBAbc : VU0_ACC_BC_ALL<"vsuba", 0b000011111>;

  // VMULAbc: ACC = fs * ft[bc] (opcode = 00110 1111 = 0x6F)
  defm VMULAbc : VU0_ACC_BC_ALL<"vmula", 0b001101111>;

  // VMADDAbc: ACC = ACC + fs * ft[bc] (opcode = 00010 1111 = 0x2F)
  defm VMADDAbc
      : VU0_ACC_BC_ALL<"vmadda", 0b000101111, [VU0ACC]>;

  // VMSUBAbc: ACC = ACC - fs * ft[bc] (opcode = 00011 1111 = 0x3F)
  defm VMSUBAbc
      : VU0_ACC_BC_ALL<"vmsuba", 0b000111111, [VU0ACC]>;

  // Broadcast operations: fs op ft.<field>. A broadcast on its own is added
  // to zero.
  foreach F = [["x", "0"], ["y", "1"], ["z", "2"], ["w", "3"]] in {
    defvar Field = TImmLeaf<i32, "return Imm == " # F[1] # ";">;
    defvar BC = F[0];
    foreach Op = [["ADD", "fadd"], ["SUB", "fsub"], ["MAX", "fmaxnum"],
                  ["MINI", "fminnum"], ["MUL", "fmul"]] in
      def : Pat<(!cast<SDNode>(Op[1]) v4f32:$fs,
                                      (MipsVU0BCAST v4f32:$ft, Field)),
                (!cast<Instruction>("V" # Op[0] # "bc" # BC) 15, v4f32:$fs,
                                                             v4f32:$ft)>;
    def : Pat<(MipsVU0BCAST v4f32:$ft, Field),
              (!cast<Instruction>("VADDbc" # BC) 15,
                   (VSUB 15, v4f32:$ft, v4f32:$ft), v4f32:$ft)>;
  }

  //===--------------------------------------------------------------------===//
  // VU0 Q-register operations
  //===--------------------------------------------------------------------===//

  // VWAITQ - Wait for Q register computation to complete
  // Encoding: COP2 (010010) + dest=0000 + 00000 00000 + special(01110111111)
  def VWAITQ
      : InstSE<(outs), (ins), "vwaitq", [], FrmR, "vwaitq"> {
    let Inst{31 -26} = 0b010010;               // COP2
    let Inst{25} = 1;                          // CO bit
    let Inst{24 -21} = 0b0000;                 // dest
    let Inst{20 -0} = 0b000000000001110111111; // special + 01110111111
    let hasSideEffects = 1;
    let Uses = [VU0Q];
    let Predicates = [HasVU0];
  }

  // VNOP - VU0 NOP (no operation)
  // Encoding: COP2 (010010) + dest=0000 + 00000 00000 + special(01011111111)
  def VNOP : InstSE<(outs), (ins), "vnop", [], FrmR, "vnop"> {
    let Inst{31 -26} = 0b010010;               // COP2
    let Inst{25} = 1;                          // CO bit
    let Inst{24 -21} = 0b0000;                 // dest
    let Inst{20 -0} = 0b000000000001011111111; // special + 01011111111
    let hasSideEffects = 0;
    let Predicates = [HasVU0];
  }

  // VMULQ - Multiply by Q register: fd = fs * Q
  // This is a 2-register instruction where one source is implicitly Q
  // Encoding: COP2 + dest + ft=0 + fs + fd + special(011100)
  class VU0_2R_Q<string opstr, bits<6> funct>
      : InstSE<
            (outs VFOpnd:$fd), (ins vu0destmask:$dest, VFOpnd:$fs, VU0QOpnd:$q),
            !strconcat(opstr, "$dest\t$fd, $fs, $q"), [], FrmR, opstr> {
    bits<5> fd;
    bits<5> fs;
    bits<4> dest;

    let Inst{31 -26} = 0b010010; // COP2
    let Inst{25} = 1;            // CO bit
    let Inst{24 -21} = dest;     // destination mask
    let Inst{20 -16} = 0;        // ft = 0 for Q operations
    let Inst{15 -11} = fs;
    let Inst{10 -6} = fd;
    let Inst{5 -0} = funct;
  }

  def VMULQ : VU0_2R_Q<"vmulq", 0b011100>;
  def VADDQ : VU0_2R_Q<"vaddq", 0b100000>;
  def VSUBQ : VU0_2R_Q<"vsubq", 0b100100>;
  def VMADDQ : VU0_2R_Q<"vmaddq", 0b100001> {
    let Uses = [VU0ACC];
  }
  def VMSUBQ : VU0_2R_Q<"vmsubq", 0b100101> {
    let Uses = [VU0ACC];
  }

  //===--------------------------------------------------------------------===//
  // VU0 Outer Product Instructions
  //===--------------------------------------------------------------------===//

  // VOPMULA - Outer product multiply-add to ACC
  // ACC = fs x ft (cross product like operation)
  // Encoding: COP2 + dest + ft + fs + special(01011111110)
  def VOPMULA
      : InstSE<(outs),
               (ins vu0destmask:$dest, VU0AccOpnd:$accd, VFOpnd:$fs, VFOpnd:$ft),
               "vopmula$dest\t$accd, $fs, $ft", [], FrmR,
               "vopmula"> {
    bits<5> fs;
    bits<5> ft;
    bits<4> dest;

    let Inst{31 -26} = 0b010010; // COP2
    let Inst{25} = 1;            // CO bit
    let Inst{24 -21} = dest;     // destination mask
    let Inst{20 -16} = ft;
    let Inst{15 -11} = fs;
    let Inst{10 -0} = 0b01011111110; // special opcode
    let Defs = [VU0ACC];
  }

  // VOPMSUB - Outer product multiply-subtract
  // fd = ACC - fs x ft (cross product like operation)
  // Encoding: COP2 + dest + ft + fs + fd + special(0101111)
  def VOPMSUB : InstSE<(outs VFOpnd:$fd),
                       (ins vu0destmask:$dest, VFOpnd:$fs, VFOpnd:$ft),
                       "vopmsub$dest\t$fd, $fs, $ft", [], FrmR,
                       "vopmsub"> {
    bits<5> fd;
    bits<5> fs;
    bits<5> ft;
    bits<4> dest;

    let Inst{31 -26} = 0b010010; // COP2
    let Inst{25} = 1;            // CO bit
    let Inst{24 -21} = dest;     // destination mask
    let Inst{20 -16} = ft;
    let Inst{15 -11} = fs;
    let Inst{10 -6} = fd;
    let Inst{5 -0} = 0b101110; // special opcode
    let Uses = [VU0ACC];
  }

  //===--------------------------------------------------------------------===//
  // VU0 Clip Detection
  //===--------------------------------------------------------------------===//

  // VCLIPW - Clip detection
  // Tests fs.xyz against ft.w and updates clip flags
  // Encoding: COP2 + dest + ft + fs + special(00111111111)
  def VCLIPW
      : InstSE<(outs), (ins vu0destmask:$dest, VFOpnd:$fs, VFOpnd:$ft),
               "vclipw$dest\t$fs, $ft", [], FrmR, "vclipw"> {
    bits<5> fs;
    bits<5> ft;
    bits<4> dest;

    let Inst{31 -26} = 0b010010; // COP2
    let Inst{25} = 1;            // CO bit
    let Inst{24 -21} = dest;     // destination mask
    let Inst{20 -16} = ft;
    let Inst{15 -11} = fs;
    let Inst{10 -0} = 0b00111111111; // special opcode
    let hasSideEffects = 1;
  }

  //===--------------------------------------------------------------------===//
  // VU0 Division/Square Root Instructions
  // These use field selector syntax: $vf0w means vf0's w component
  // Parser creates TWO operands per VF+field: (field_imm, vf_register)
  // Asm string concatenates them: $fs$fsf prints as "$vf0w"
  //===--------------------------------------------------------------------===//

  // VDIV - Floating divide: Q = fs[fsf] / ft[ftf]
  // MacroOP field type 4: COP2 + co + ftf + fsf + ft + fs + opcode
  // Encoding: 010010 1 ftf(2) fsf(2) ft(5) fs(5) 01110111100
  // VFWithFieldOpnd is a VF register and a field ($vf0w).
  def VDIV : InstSE<(outs VU0QOpnd:$q),
                    (ins VFWithFieldOpnd:$src1, VFWithFieldOpnd:$src2),
                    "vdiv\t$q, $src1, $src2", [], FrmR, "vdiv"> {
    bits<0> q;
    bits<7> src1; // Combined: fsf(2) + fs(5)
    bits<7> src2; // Combined: ftf(2) + ft(5)

    let Inst{31 -26} = 0b010010;     // COP2
    let Inst{25} = 1;                // CO bit
    let Inst{24 -23} = src2{6 -5};   // ftf - field selector for ft
    let Inst{22 -21} = src1{6 -5};   // fsf - field selector for fs
    let Inst{20 -16} = src2{4 -0};   // ft register
    let Inst{15 -11} = src1{4 -0};   // fs register
    let Inst{10 -0} = 0b01110111100; // VDIV opcode
    let hasSideEffects = 1;
  }

  // VRSQRT - Reciprocal square root: Q = fs[fsf] / sqrt(ft[ftf])
  // Encoding: 010010 1 ftf(2) fsf(2) ft(5) fs(5) 01110111110
  def VRSQRT : InstSE<(outs VU0QOpnd:$q),
                      (ins VFWithFieldOpnd:$src1, VFWithFieldOpnd:$src2),
                      "vrsqrt\t$q, $src1, $src2", [], FrmR, "vrsqrt"> {
    bits<0> q;
    bits<7> src1; // Combined: fsf(2) + fs(5)
    bits<7> src2; // Combined: ftf(2) + ft(5)

    let Inst{31 -26} = 0b010010;     // COP2
    let Inst{25} = 1;                // CO bit
    let Inst{24 -23} = src2{6 -5};   // ftf - field selector for ft
    let Inst{22 -21} = src1{6 -5};   // fsf - field selector for fs
    let Inst{20 -16} = src2{4 -0};   // ft register
    let Inst{15 -11} = src1{4 -0};   // fs register
    let Inst{10 -0} = 0b01110111110; // VRSQRT opcode
    let hasSideEffects = 1;
  }

  // VSQRT - Square root: Q = sqrt(ft[ftf])
  // Only uses ft register, fs is fixed to 0
  // Encoding: 010010 1 ftf(2) 00 ft(5) 00000 01110111101
  def VSQRT : InstSE<(outs VU0QOpnd:$q), (ins VFWithFieldOpnd:$src),
                     "vsqrt\t$q, $src", [], FrmR, "vsqrt"> {
    bits<0> q;
    bits<7> src; // Combined: ftf(2) + ft(5)

    let Inst{31 -26} = 0b010010;     // COP2
    let Inst{25} = 1;                // CO bit
    let Inst{24 -23} = src{6 -5};    // ftf - field selector for ft
    let Inst{22 -21} = 0b00;         // fsf = 0 (unused)
    let Inst{20 -16} = src{4 -0};    // ft register
    let Inst{15 -11} = 0b00000;      // fs = 0 (unused)
    let Inst{10 -0} = 0b01110111101; // VSQRT opcode
    let hasSideEffects = 1;
  }

  //===--------------------------------------------------------------------===//
  // VU0 Control Register Transfer (CFC2/CTC2)
  // These transfer data between GPRs and VU0 control registers (VI16-VI31)
  // Control registers include Status flag, MAC flag, Clip flag, etc.
  //===--------------------------------------------------------------------===//

  // CFC2 - Copy From Coprocessor 2 Control: GPR = VU0_CR
  // Reads from VU0 control register (VI16-VI31) to GPR
  def CFC2 : InstSE<(outs GPR32Opnd:$rt), (ins COP2Opnd:$rd),
                    "cfc2\t$rt, $rd", [], FrmFR, "cfc2">,
             MFC2_FM<0b00010> {
    let DecoderNamespace = "R5900";
  }

  // CTC2 - Copy To Coprocessor 2 Control: VU0_CR = GPR
  // Writes to VU0 control register (VI16-VI31) from GPR
  def CTC2 : InstSE<(outs COP2Opnd:$rd), (ins GPR32Opnd:$rt),
                    "ctc2\t$rt, $rd", [], FrmFR, "ctc2">,
             MFC2_FM<0b00110> {
    let DecoderNamespace = "R5900";
  }

  // QMFC2 - Quadword Move From Coprocessor 2: GPR128 = VF
  // Transfers 128-bit data from VF register to GPR128
  // Encoding: COP2(6) | 00001(5) | rt(5) | rd(5) | 0(11)
  def QMFC2 : InstSE<(outs GPR128Opnd:$rt), (ins VFOpnd:$rd),
                     "qmfc2\t$rt, $rd", [], FrmFR, "qmfc2">,
              MFC2_FM<0b00001> {
    let DecoderNamespace = "R5900";
  }

  // QMTC2 - Quadword Move To Coprocessor 2: VF = GPR128
  // Transfers 128-bit data from GPR128 to VF register
  // Encoding: COP2(6) | 00101(5) | rt(5) | rd(5) | 0(11)
  def QMTC2 : InstSE<(outs VFOpnd:$rd), (ins GPR128Opnd:$rt),
                     "qmtc2\t$rt, $rd", [], FrmFR, "qmtc2">,
              MFC2_FM<0b00101> {
    let DecoderNamespace = "R5900";
  }

  // The interlocked moves (.i): they wait for a VU0 micro program to end
  // first (a micro program and macro code share the VU0 registers).
  let DecoderNamespace = "R5900", hasSideEffects = 1 in {
  def CFC2_I : InstSE<(outs GPR32Opnd:$rt), (ins COP2Opnd:$rd),
                      "cfc2.i\t$rt, $rd", [], FrmFR, "cfc2.i">,
               MFC2_FM<0b00010, 1>;
  def CTC2_I : InstSE<(outs COP2Opnd:$rd), (ins GPR32Opnd:$rt),
                      "ctc2.i\t$rt, $rd", [], FrmFR, "ctc2.i">,
               MFC2_FM<0b00110, 1>;
  def QMFC2_I : InstSE<(outs GPR128Opnd:$rt), (ins VFOpnd:$rd),
                       "qmfc2.i\t$rt, $rd", [], FrmFR, "qmfc2.i">,
                MFC2_FM<0b00001, 1>;
  def QMTC2_I : InstSE<(outs VFOpnd:$rd), (ins GPR128Opnd:$rt),
                       "qmtc2.i\t$rt, $rd", [], FrmFR, "qmtc2.i">,
                MFC2_FM<0b00101, 1>;
  }

  // Bitcast patterns between v4f32 (VF register) and v4i32 (GPR128)
  // Used for EXTRACT_VECTOR_ELT and INSERT_VECTOR_ELT lowering
  def : Pat<(v4i32 (bitconvert v4f32:$src)), (QMFC2 v4f32:$src)>;
  def : Pat<(v4f32 (bitconvert v4i32:$src)), (QMTC2 v4i32:$src)>;

  // v4i32 lives in a 128-bit GPR, so a conversion moves it to or from VU0.
  def : Pat<(v4i32 (fp_to_sint v4f32:$src)), (QMFC2 (VFTOI0 15, v4f32:$src))>;
  def : Pat<(v4f32 (sint_to_fp v4i32:$src)), (VITOF0 15, (QMTC2 v4i32:$src))>;

  // Fixed-point conversions with n fraction bits, combined from a
  // multiplication by 2^n or 2^-n.
  foreach N = [4, 12, 15] in {
    defvar Bits = TImmLeaf<i32, "return Imm == " # N # ";">;
    def : Pat<(v4i32 (MipsVU0FTOI v4f32:$src, Bits)),
              (QMFC2 (!cast<Instruction>("VFTOI" # N) 15, v4f32:$src))>;
    def : Pat<(v4f32 (MipsVU0ITOF v4i32:$src, Bits)),
              (!cast<Instruction>("VITOF" # N) 15, (QMTC2 v4i32:$src))>;
  }

} // Predicates = [HasVU0]
