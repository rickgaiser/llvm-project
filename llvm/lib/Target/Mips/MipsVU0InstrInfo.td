//===- MipsVU0InstrInfo.td - VU0 Macro Mode Instructions ----*- tablegen -*-===//
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

class LW_VF<string opstr, RegisterOperand RC, InstrItinClass Itin,
            SDPatternOperator OpNode= null_frag> :
  InstSE<(outs RC:$rt), (ins mem_simm16:$addr),
         !strconcat(opstr, "\t$rt, $addr"),
         [(set RC:$rt, (OpNode addrDefault:$addr))], Itin, FrmFI, opstr> {
  let DecoderMethod = "DecodeVFMem";
  let DecoderNamespace = "R5900";
  let mayLoad = 1;
}

class SW_VF<string opstr, RegisterOperand RC, InstrItinClass Itin,
            SDPatternOperator OpNode= null_frag> :
  InstSE<(outs), (ins RC:$rt, mem_simm16:$addr),
         !strconcat(opstr, "\t$rt, $addr"),
         [(OpNode RC:$rt, addrDefault:$addr)], Itin, FrmFI, opstr> {
  let DecoderMethod = "DecodeVFMem";
  let DecoderNamespace = "R5900";
  let mayStore = 1;
}

let Predicates = [HasVU0] in {
  // LQC2 - Load Quadword to COP2 (128-bit memory -> VF register)
  def LQC2 : LW_VF<"lqc2", VFOpnd, II_LQC2, load>, LW_FM<0x36>;

  // SQC2 - Store Quadword from COP2 (VF register -> 128-bit memory)
  def SQC2 : SW_VF<"sqc2", VFOpnd, II_SQC2, store>, LW_FM<0x3e>;
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
// The ACC is always implicit in the encoding, but assembly may specify it explicitly
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

// VF register with field selector - combines VF register and field in one operand
// Used for instructions like VDIV/VRSQRT that use syntax: $vf0w
// The parser extracts both the register and field from the combined syntax
// This creates TWO MC operands: field selector (2-bit immediate) + VF register
def VFWithFieldAsmOperand : AsmOperandClass {
  let Name = "VFWithField";
  let RenderMethod = "addVFWithFieldOperands";
  let ParserMethod = "parseVFWithField";
}

// Composite operand: VF register + field selector encoded as 7-bit immediate
// Bits 6-5: field (0=x, 1=y, 2=z, 3=w)
// Bits 4-0: VF register index (0-31)
// Parser creates composite operand, RenderMethod adds single immediate
def VFWithFieldOpnd : Operand<i32> {
  let ParserMatchClass = VFWithFieldAsmOperand;
  let PrintMethod = "printVFWithField";
}

//===----------------------------------------------------------------------===//
// VU0 SDNodes
//===----------------------------------------------------------------------===//

// SDTypeProfile for VU0 masked operations: (result, base, other, mask)
// result[i] = (mask[i] ? op(base[i], other[i]) : base[i])
def SDT_VU0Masked : SDTypeProfile<1, 3, [
  SDTCisVT<0, v4f32>, SDTCisVT<1, v4f32>, SDTCisVT<2, v4f32>, SDTCisVT<3, i32>
]>;

// SDNodes for VU0 masked operations (blend folded into arithmetic)
def MipsVU0BLEND        : SDNode<"MipsISD::VU0_BLEND", SDT_VU0Masked>;
def MipsVU0FADD_MASKED  : SDNode<"MipsISD::VU0_FADD_MASKED", SDT_VU0Masked>;
def MipsVU0FSUB_MASKED  : SDNode<"MipsISD::VU0_FSUB_MASKED", SDT_VU0Masked>;
def MipsVU0FMUL_MASKED  : SDNode<"MipsISD::VU0_FMUL_MASKED", SDT_VU0Masked>;

// SDTypeProfile for VU0 broadcast operations: 2 v4f32 inputs, 1 v4f32 output
def SDT_VU0Broadcast : SDTypeProfile<1, 2, [
  SDTCisVT<0, v4f32>, SDTCisVT<1, v4f32>, SDTCisVT<2, v4f32>
]>;

// SDNodes for VU0 broadcast multiply operations
// VMULbc: fd = fs * ft[bc]
def MipsVU0MULx : SDNode<"MipsISD::VU0_MULx", SDT_VU0Broadcast>;
def MipsVU0MULy : SDNode<"MipsISD::VU0_MULy", SDT_VU0Broadcast>;
def MipsVU0MULz : SDNode<"MipsISD::VU0_MULz", SDT_VU0Broadcast>;
def MipsVU0MULw : SDNode<"MipsISD::VU0_MULw", SDT_VU0Broadcast>;
// VMADDbc: fd = ACC + fs * ft[bc]
def MipsVU0MADDx : SDNode<"MipsISD::VU0_MADDx", SDT_VU0Broadcast>;
def MipsVU0MADDy : SDNode<"MipsISD::VU0_MADDy", SDT_VU0Broadcast>;
def MipsVU0MADDz : SDNode<"MipsISD::VU0_MADDz", SDT_VU0Broadcast>;
def MipsVU0MADDw : SDNode<"MipsISD::VU0_MADDw", SDT_VU0Broadcast>;
// VMULAbc: ACC = fs * ft[bc]
def MipsVU0MULAx : SDNode<"MipsISD::VU0_MULAx", SDT_VU0Broadcast>;
def MipsVU0MULAy : SDNode<"MipsISD::VU0_MULAy", SDT_VU0Broadcast>;
def MipsVU0MULAz : SDNode<"MipsISD::VU0_MULAz", SDT_VU0Broadcast>;
def MipsVU0MULAw : SDNode<"MipsISD::VU0_MULAw", SDT_VU0Broadcast>;

//===----------------------------------------------------------------------===//
// VU0 Instruction Classes
//===----------------------------------------------------------------------===//
//
// Instruction classes:
// - VU0_Type1: 3 VF registers + 6-bit opcode (VADD, VSUB, VMUL, VMADD, VMSUB)
// - VU0_Type0: 3 VF registers + 4-bit opcode + 2-bit bc (VADDbc, VSUBbc, VMULbc)
// - VU0_Type3: 2 VF registers + 11-bit opcode (VADDA, VSUBA, VMULA - ACC dest)
// - VU0_Type2: 2 VF registers + 9-bit opcode + 2-bit bc (VADDAbc, etc.)

// VU0 Type 1 instruction class: 3 VF registers, 6-bit opcode, variable dest mask
// Used for VADD, VSUB, VMUL, VMADD, VMSUB - assembly visible with variable mask
class VU0_3R<string opstr, bits<6> funct, InstrItinClass Itin>
    : InstSE<(outs VFOpnd:$fd), (ins vu0destmask:$dest, VFOpnd:$fs, VFOpnd:$ft),
             !strconcat(opstr, "$dest\t$fd, $fs, $ft"),
             [], Itin, FrmR, opstr> {
  let DecoderNamespace = "R5900";
  let hasSideEffects = 0;
  bits<5> fd;
  bits<5> fs;
  bits<5> ft;
  bits<4> dest;
  bits<32> Inst;
  let Inst{31-26} = 0b010010;  // COP2
  let Inst{25}    = 1;         // co bit
  let Inst{24-21} = dest;      // Variable destination mask from operand
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Inst{10-6}  = fd;
  let Inst{5-0}   = funct;
}

// VU0 Type 1 ISel variant: hardcoded xyzw mask for code generation
class VU0_3R_ISel<string opstr, bits<6> funct, InstrItinClass Itin,
                  SDPatternOperator OpNode>
    : InstSE<(outs VFOpnd:$fd), (ins VFOpnd:$fs, VFOpnd:$ft),
             !strconcat(opstr, ".xyzw\t$fd, $fs, $ft"),
             [(set VFOpnd:$fd, (OpNode VFOpnd:$fs, VFOpnd:$ft))],
             Itin, FrmR, opstr> {
  let isCodeGenOnly = 1;
  let DecoderNamespace = "R5900";
  let hasSideEffects = 0;
  bits<5> fd;
  bits<5> fs;
  bits<5> ft;
  bits<32> Inst;
  let Inst{31-26} = 0b010010;  // COP2
  let Inst{25}    = 1;         // co bit
  let Inst{24-21} = 0b1111;    // dest = xyzw (all lanes)
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Inst{10-6}  = fd;
  let Inst{5-0}   = funct;
}

// VU0 Type 3 instruction class: 2 VF registers, ACC destination, 11-bit opcode
// Used for VADDA, VSUBA, VMULA, VMADDA, VMSUBA - assembly visible with variable mask
// ACC operand is explicit in assembly but implicit in encoding (always ACC)
class VU0_ACC<string opstr, bits<11> funct, InstrItinClass Itin,
              list<Register> UseRegs = []>
    : InstSE<(outs), (ins vu0destmask:$dest, VU0AccOpnd:$acc, VFOpnd:$fs, VFOpnd:$ft),
             !strconcat(opstr, "$dest\t$acc, $fs, $ft"),
             [], Itin, FrmR, opstr> {
  let DecoderNamespace = "R5900";
  let Defs = [VU0ACC];
  let Uses = UseRegs;
  let hasSideEffects = 0;
  bits<5> fs;
  bits<5> ft;
  bits<4> dest;
  bits<32> Inst;
  let Inst{31-26} = 0b010010;  // COP2
  let Inst{25}    = 1;         // co bit
  let Inst{24-21} = dest;      // Variable destination mask from operand
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Inst{10-0}  = funct;
}

// VU0 Type 1 with ACC read: 3 VF registers, uses ACC, 6-bit opcode
// Used for VMADD, VMSUB (reads ACC, writes VF) - assembly visible with variable mask
class VU0_3R_ACC<string opstr, bits<6> funct, InstrItinClass Itin>
    : InstSE<(outs VFOpnd:$fd), (ins vu0destmask:$dest, VFOpnd:$fs, VFOpnd:$ft),
             !strconcat(opstr, "$dest\t$fd, $fs, $ft"),
             [], Itin, FrmR, opstr> {
  let DecoderNamespace = "R5900";
  let Uses = [VU0ACC];
  let hasSideEffects = 0;
  bits<5> fd;
  bits<5> fs;
  bits<5> ft;
  bits<4> dest;
  bits<32> Inst;
  let Inst{31-26} = 0b010010;  // COP2
  let Inst{25}    = 1;         // co bit
  let Inst{24-21} = dest;      // Variable destination mask from operand
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Inst{10-6}  = fd;
  let Inst{5-0}   = funct;
}

// VU0 Type 1 with dynamic destination mask: 3 VF registers, variable mask
// Used for masked VADD, VSUB, VMUL (blend operations)
class VU0_3R_Masked<string opstr, bits<6> funct, InstrItinClass Itin>
    : InstSE<(outs VFOpnd:$fd), (ins VFOpnd:$base, VFOpnd:$fs, VFOpnd:$ft, vu0destmask:$mask),
             !strconcat(opstr, "$mask\t$fd, $fs, $ft"),
             [], Itin, FrmR, opstr> {
  let DecoderNamespace = "R5900";
  let hasSideEffects = 0;
  let isCodeGenOnly = 1;  // Not for assembly, only for ISel
  bits<5> fd;
  bits<5> fs;
  bits<5> ft;
  bits<4> mask;
  bits<32> Inst;
  let Inst{31-26} = 0b010010;  // COP2
  let Inst{25}    = 1;         // co bit
  let Inst{24-21} = mask;      // Variable destination mask from operand
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Inst{10-6}  = fd;
  let Inst{5-0}   = funct;
  let Constraints = "$base = $fd";
}

// VU0 VMOVE instruction class: 2 VF registers, masked move
// VMOVE.dest ft, fs - copies selected lanes from fs to ft
// Note: ft is the destination (not fd like in arithmetic ops)
// Encoding: Type 3 format with 11-bit opcode = 0x33C
class VU0_MOVE_Masked<string opstr, bits<11> funct, InstrItinClass Itin>
    : InstSE<(outs VFOpnd:$ft), (ins VFOpnd:$base, VFOpnd:$fs, vu0destmask:$mask),
             !strconcat(opstr, "$mask\t$ft, $fs"),
             [], Itin, FrmR, opstr> {
  let DecoderNamespace = "R5900";
  let hasSideEffects = 0;
  let isCodeGenOnly = 1;  // Not for assembly, only for ISel
  bits<5> ft;
  bits<5> fs;
  bits<4> mask;
  bits<32> Inst;
  let Inst{31-26} = 0b010010;  // COP2
  let Inst{25}    = 1;         // co bit
  let Inst{24-21} = mask;      // Variable destination mask from operand
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Inst{10-0}  = funct;
  let Constraints = "$base = $ft";
}

// VU0 Type 3 instruction class: 2 VF registers (ft=dest, fs=src), 11-bit opcode
// Used for VABS, VMOVE, etc. - variable dest mask
class VU0_2R<string opstr, bits<11> funct, InstrItinClass Itin>
    : InstSE<(outs VFOpnd:$ft), (ins vu0destmask:$dest, VFOpnd:$fs),
             !strconcat(opstr, "$dest\t$ft, $fs"),
             [], Itin, FrmR, opstr> {
  let DecoderNamespace = "R5900";
  let hasSideEffects = 0;
  bits<5> ft;
  bits<5> fs;
  bits<4> dest;
  bits<32> Inst;
  let Inst{31-26} = 0b010010;  // COP2
  let Inst{25}    = 1;         // co bit
  let Inst{24-21} = dest;      // Variable destination mask from operand
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Inst{10-0}  = funct;
}

// VU0 Type 3 ISel variant: hardcoded xyzw mask for code generation
class VU0_2R_ISel<string opstr, bits<11> funct, InstrItinClass Itin,
                  SDPatternOperator OpNode>
    : InstSE<(outs VFOpnd:$ft), (ins VFOpnd:$fs),
             !strconcat(opstr, ".xyzw\t$ft, $fs"),
             [(set VFOpnd:$ft, (OpNode VFOpnd:$fs))],
             Itin, FrmR, opstr> {
  let isCodeGenOnly = 1;
  let DecoderNamespace = "R5900";
  let hasSideEffects = 0;
  bits<5> ft;
  bits<5> fs;
  bits<32> Inst;
  let Inst{31-26} = 0b010010;  // COP2
  let Inst{25}    = 1;         // co bit
  let Inst{24-21} = 0b1111;    // dest = xyzw (all lanes)
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Inst{10-0}  = funct;
}

let Predicates = [HasVU0] in {
  // VU0 Vector Add/Subtract/Multiply (3-register forms, variable dest mask)
  // Assembly-visible instructions with variable mask operand
  let isCommutable = 1 in {
    def VADD  : VU0_3R<"vadd", 0b101000, II_VU0_VADD>;   // 0x28
    def VMUL  : VU0_3R<"vmul", 0b101010, II_VU0_VMUL>;   // 0x2A
  }
  def VSUB  : VU0_3R<"vsub", 0b101100, II_VU0_VSUB>;   // 0x2C

  // ISel-only variants with hardcoded xyzw mask for code generation patterns
  let isCommutable = 1 in {
    def VADD_ISel : VU0_3R_ISel<"vadd", 0b101000, II_VU0_VADD, fadd>;
    def VMUL_ISel : VU0_3R_ISel<"vmul", 0b101010, II_VU0_VMUL, fmul>;
  }
  def VSUB_ISel : VU0_3R_ISel<"vsub", 0b101100, II_VU0_VSUB, fsub>;

  // VU0 Vector Min/Max (3-register forms)
  let isCommutable = 1 in {
    def VMAX  : VU0_3R<"vmax", 0b101011, II_VU0_VMAX>;   // 0x2B
    def VMINI : VU0_3R<"vmini", 0b101111, II_VU0_VMINI>; // 0x2F
    // ISel variants with fixed xyzw mask for patterns
    def VMAX_ISel  : VU0_3R_ISel<"vmax", 0b101011, II_VU0_VMAX, null_frag>;
    def VMINI_ISel : VU0_3R_ISel<"vmini", 0b101111, II_VU0_VMINI, null_frag>;
  }

  // VU0 Absolute value (2-register form: ft = |fs|)
  def VABS  : VU0_2R<"vabs", 0b01111111101, II_VU0_VABS>;  // 0x1FD
  def VABS_ISel : VU0_2R_ISel<"vabs", 0b01111111101, II_VU0_VABS, fabs>;

  // VU0 VMOVE (move) - opcode 0x33C
  // VMOVE.dest ft, fs - copies selected lanes from fs to ft
  def VMOVE : VU0_2R<"vmove", 0b01100111100, II_VU0_VMOVE>;  // 0x33C

  // VU0 VMR32 (move rotate 32) - opcode 0x33D
  // VMR32.dest ft, fs - rotates components: ft.x=fs.y, ft.y=fs.z, ft.z=fs.w, ft.w=fs.x
  def VMR32 : VU0_2R<"vmr32", 0b01100111101, II_VU0_VMOVE>;  // 0x33D

  // VU0 Multiply-Accumulate (uses ACC register)
  def VMADD : VU0_3R_ACC<"vmadd", 0b101001, II_VU0_VMADD>;  // 0x29
  def VMSUB : VU0_3R_ACC<"vmsub", 0b101101, II_VU0_VMSUB>;  // 0x2D

  // VU0 ACC-destination instructions (writes to ACC)
  def VADDA : VU0_ACC<"vadda", 0b10111111100, II_VU0_VADDA>;   // bits 10-0 = 0x5FC
  def VSUBA : VU0_ACC<"vsuba", 0b10111111101, II_VU0_VSUBA>;   // bits 10-0 = 0x5FD
  def VMULA : VU0_ACC<"vmula", 0b10111111110, II_VU0_VMULA>;   // bits 10-0 = 0x5FE

  // VU0 Multiply-Add/Sub to ACC (reads and writes ACC)
  def VMADDA : VU0_ACC<"vmadda", 0b10111111111, II_VU0_VMADDA, [VU0ACC]>;  // 0x5FF
  def VMSUBA : VU0_ACC<"vmsuba", 0b10111111011, II_VU0_VMSUBA, [VU0ACC]>;  // 0x5FB

  //===----------------------------------------------------------------------===//
  // VU0 Masked Instructions (dynamic destination mask)
  //===----------------------------------------------------------------------===//
  // These instructions have a variable destination mask for blend operations.
  // The mask is an immediate operand that selects which components to write.

  def VADDm : VU0_3R_Masked<"vadd", 0b101000, II_VU0_VADD>;  // 0x28
  def VSUBm : VU0_3R_Masked<"vsub", 0b101100, II_VU0_VSUB>;  // 0x2C
  def VMULm : VU0_3R_Masked<"vmul", 0b101010, II_VU0_VMUL>;  // 0x2A

  // VU0 VMOVE (masked move) - opcode 0x33C
  // VMOVE.dest ft, fs - copies selected lanes from fs to ft
  def VMOVEm : VU0_MOVE_Masked<"vmove", 0b01100111100, II_VU0_VMOVE>;  // 0x33C

  // Patterns for masked operations
  // Match: (VU0_FADD_MASKED base, other, mask) -> VADDm(base, base, other, mask)
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

  // Patterns for VU0 min/max operations
  // Note: VU0 min/max don't follow IEEE NaN semantics, use with -ffast-math
  def : Pat<(fmaxnum v4f32:$fs, v4f32:$ft), (VMAX_ISel VFRegs:$fs, VFRegs:$ft)>;
  def : Pat<(fminnum v4f32:$fs, v4f32:$ft), (VMINI_ISel VFRegs:$fs, VFRegs:$ft)>;

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
class VU0_3R_BC<string opstr, string suffix, bits<4> opcode, bits<2> bc,
                InstrItinClass Itin>
    : InstSE<(outs VFOpnd:$fd), (ins vu0destmask:$dest, VFOpnd:$fs, VFOpnd:$ft),
             !strconcat(opstr, suffix, "$dest\t$fd, $fs, $ft"),
             [], Itin, FrmR, opstr> {
  let DecoderNamespace = "R5900";
  let hasSideEffects = 0;
  bits<5> fd;
  bits<5> fs;
  bits<5> ft;
  bits<4> dest;
  bits<32> Inst;
  let Inst{31-26} = 0b010010;  // COP2
  let Inst{25}    = 1;         // co bit
  let Inst{24-21} = dest;      // Variable destination mask from operand
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Inst{10-6}  = fd;
  let Inst{5-2}   = opcode;
  let Inst{1-0}   = bc;
}

// VU0 Type 0 with ACC read: 3 VF registers + 4-bit opcode + 2-bit bc
// Used for VMADDbc, VMSUBbc (reads ACC, writes VF) - variable dest mask
class VU0_3R_BC_ACC<string opstr, string suffix, bits<4> opcode, bits<2> bc,
                    InstrItinClass Itin>
    : InstSE<(outs VFOpnd:$fd), (ins vu0destmask:$dest, VFOpnd:$fs, VFOpnd:$ft),
             !strconcat(opstr, suffix, "$dest\t$fd, $fs, $ft"),
             [], Itin, FrmR, opstr> {
  let DecoderNamespace = "R5900";
  let Uses = [VU0ACC];
  let hasSideEffects = 0;
  bits<5> fd;
  bits<5> fs;
  bits<5> ft;
  bits<4> dest;
  bits<32> Inst;
  let Inst{31-26} = 0b010010;  // COP2
  let Inst{25}    = 1;         // co bit
  let Inst{24-21} = dest;      // Variable destination mask from operand
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Inst{10-6}  = fd;
  let Inst{5-2}   = opcode;
  let Inst{1-0}   = bc;
}

// VU0 Type 2 instruction class: 2 VF registers, ACC dest, 9-bit opcode + 2-bit bc
// Used for VADDAbc, VSUBAbc, VMULAbc, VMADDAbc, VMSUBAbc - variable dest mask
// ACC operand is explicit in assembly but implicit in encoding
class VU0_ACC_BC<string opstr, string suffix, bits<9> opcode, bits<2> bc,
                 InstrItinClass Itin, list<Register> UseRegs = []>
    : InstSE<(outs), (ins vu0destmask:$dest, VU0AccOpnd:$acc, VFOpnd:$fs, VFOpnd:$ft),
             !strconcat(opstr, suffix, "$dest\t$acc, $fs, $ft"),
             [], Itin, FrmR, opstr> {
  let DecoderNamespace = "R5900";
  let Defs = [VU0ACC];
  let Uses = UseRegs;
  let hasSideEffects = 0;
  bits<5> fs;
  bits<5> ft;
  bits<4> dest;
  bits<32> Inst;
  let Inst{31-26} = 0b010010;  // COP2
  let Inst{25}    = 1;         // co bit
  let Inst{24-21} = dest;      // Variable destination mask from operand
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Inst{10-2}  = opcode;
  let Inst{1-0}   = bc;
}

// VU0 Type 0 ISel variant: hardcoded xyzw mask for code generation
class VU0_3R_BC_ISel<string opstr, string suffix, bits<4> opcode, bits<2> bc,
                     InstrItinClass Itin, SDPatternOperator OpNode = null_frag>
    : InstSE<(outs VFOpnd:$fd), (ins VFOpnd:$fs, VFOpnd:$ft),
             !strconcat(opstr, suffix, ".xyzw\t$fd, $fs, $ft"),
             [(set VFOpnd:$fd, (OpNode VFOpnd:$fs, VFOpnd:$ft))],
             Itin, FrmR, opstr> {
  let isCodeGenOnly = 1;
  let DecoderNamespace = "R5900";
  let hasSideEffects = 0;
  bits<5> fd;
  bits<5> fs;
  bits<5> ft;
  bits<32> Inst;
  let Inst{31-26} = 0b010010;  // COP2
  let Inst{25}    = 1;         // co bit
  let Inst{24-21} = 0b1111;    // dest = xyzw (all lanes)
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Inst{10-6}  = fd;
  let Inst{5-2}   = opcode;
  let Inst{1-0}   = bc;
}

// Multiclass to define all 4 broadcast variants (x, y, z, w) for 3-register ops
multiclass VU0_3R_BC_ALL<string opstr, bits<4> opcode, InstrItinClass Itin> {
  def x : VU0_3R_BC<opstr, "x", opcode, 0b00, Itin>;
  def y : VU0_3R_BC<opstr, "y", opcode, 0b01, Itin>;
  def z : VU0_3R_BC<opstr, "z", opcode, 0b10, Itin>;
  def w : VU0_3R_BC<opstr, "w", opcode, 0b11, Itin>;
}

// Multiclass for ISel variants of broadcast ops (with patterns)
multiclass VU0_3R_BC_ALL_ISel<string opstr, bits<4> opcode, InstrItinClass Itin,
                               SDPatternOperator OpX, SDPatternOperator OpY,
                               SDPatternOperator OpZ, SDPatternOperator OpW> {
  def x : VU0_3R_BC_ISel<opstr, "x", opcode, 0b00, Itin, OpX>;
  def y : VU0_3R_BC_ISel<opstr, "y", opcode, 0b01, Itin, OpY>;
  def z : VU0_3R_BC_ISel<opstr, "z", opcode, 0b10, Itin, OpZ>;
  def w : VU0_3R_BC_ISel<opstr, "w", opcode, 0b11, Itin, OpW>;
}

// Multiclass for 3-register ops that read ACC
multiclass VU0_3R_BC_ACC_ALL<string opstr, bits<4> opcode, InstrItinClass Itin> {
  def x : VU0_3R_BC_ACC<opstr, "x", opcode, 0b00, Itin>;
  def y : VU0_3R_BC_ACC<opstr, "y", opcode, 0b01, Itin>;
  def z : VU0_3R_BC_ACC<opstr, "z", opcode, 0b10, Itin>;
  def w : VU0_3R_BC_ACC<opstr, "w", opcode, 0b11, Itin>;
}

// Multiclass for ACC-destination broadcast ops
multiclass VU0_ACC_BC_ALL<string opstr, bits<9> opcode, InstrItinClass Itin,
                          list<Register> UseRegs = []> {
  def x : VU0_ACC_BC<opstr, "x", opcode, 0b00, Itin, UseRegs>;
  def y : VU0_ACC_BC<opstr, "y", opcode, 0b01, Itin, UseRegs>;
  def z : VU0_ACC_BC<opstr, "z", opcode, 0b10, Itin, UseRegs>;
  def w : VU0_ACC_BC<opstr, "w", opcode, 0b11, Itin, UseRegs>;
}

let Predicates = [HasVU0] in {
  // VU0 3-register broadcast instructions (Type 0)
  // VADDbc: fd = fs + ft[bc] (opcode = 0000)
  defm VADDbc : VU0_3R_BC_ALL<"vadd", 0b0000, II_VU0_VADD>;

  // VSUBbc: fd = fs - ft[bc] (opcode = 0001)
  defm VSUBbc : VU0_3R_BC_ALL<"vsub", 0b0001, II_VU0_VSUB>;

  // VMULbc: fd = fs * ft[bc] (opcode = 0110)
  defm VMULbc : VU0_3R_BC_ALL<"vmul", 0b0110, II_VU0_VMUL>;
  // ISel variants with patterns for VU0 broadcast multiply
  defm VMULbc_ISel : VU0_3R_BC_ALL_ISel<"vmul", 0b0110, II_VU0_VMUL,
                                         MipsVU0MULx, MipsVU0MULy,
                                         MipsVU0MULz, MipsVU0MULw>;

  // VMADDbc: fd = ACC + fs * ft[bc] (opcode = 0010)
  defm VMADDbc : VU0_3R_BC_ACC_ALL<"vmadd", 0b0010, II_VU0_VMADD>;

  // VMSUBbc: fd = ACC - fs * ft[bc] (opcode = 0011)
  defm VMSUBbc : VU0_3R_BC_ACC_ALL<"vmsub", 0b0011, II_VU0_VMSUB>;

  // VU0 ACC-destination broadcast instructions (Type 2)
  // Opcode format: upper 5 bits + 1111
  // VADDAbc: ACC = fs + ft[bc] (opcode = 00000 1111 = 0x0F)
  defm VADDAbc : VU0_ACC_BC_ALL<"vadda", 0b000001111, II_VU0_VADDA>;

  // VSUBAbc: ACC = fs - ft[bc] (opcode = 00001 1111 = 0x1F)
  defm VSUBAbc : VU0_ACC_BC_ALL<"vsuba", 0b000011111, II_VU0_VSUBA>;

  // VMULAbc: ACC = fs * ft[bc] (opcode = 00110 1111 = 0x6F)
  defm VMULAbc : VU0_ACC_BC_ALL<"vmula", 0b001101111, II_VU0_VMULA>;

  // VMADDAbc: ACC = ACC + fs * ft[bc] (opcode = 00010 1111 = 0x2F)
  defm VMADDAbc : VU0_ACC_BC_ALL<"vmadda", 0b000101111, II_VU0_VMADDA, [VU0ACC]>;

  // VMSUBAbc: ACC = ACC - fs * ft[bc] (opcode = 00011 1111 = 0x3F)
  defm VMSUBAbc : VU0_ACC_BC_ALL<"vmsuba", 0b000111111, II_VU0_VMSUBA, [VU0ACC]>;

  // Patterns for VMULbc are now defined in VMULbc_ISel via the multiclass

  //===--------------------------------------------------------------------===//
  // VU0 Q-register operations
  //===--------------------------------------------------------------------===//

  // VWAITQ - Wait for Q register computation to complete
  // Encoding: COP2 (010010) + dest=xyzw(1111) + 00000 00000 + special(0111011111)
  def VWAITQ : InstSE<(outs), (ins),
                      "vwaitq", [], II_VU0_VDIV, FrmR, "vwaitq"> {
    let Inst{31-26} = 0b010010;  // COP2
    let Inst{25}    = 1;         // CO bit
    let Inst{24-21} = 0b1111;    // dest = xyzw
    let Inst{20-0}  = 0b000000000001110111111;  // special + 0111011111
    let hasSideEffects = 1;
    let Uses = [VU0Q];
    let Predicates = [HasVU0];
  }

  // VNOP - VU0 NOP (no operation)
  // Encoding: COP2 (010010) + dest=xyzw(1111) + 00000 00000 + special(0101111111)
  def VNOP : InstSE<(outs), (ins),
                    "vnop", [], II_VU0_MISC, FrmR, "vnop"> {
    let Inst{31-26} = 0b010010;  // COP2
    let Inst{25}    = 1;         // CO bit
    let Inst{24-21} = 0b1111;    // dest = xyzw
    let Inst{20-0}  = 0b000000000001011111111;  // special + 0101111111
    let hasSideEffects = 0;
    let Predicates = [HasVU0];
  }

  // VMULQ - Multiply by Q register: fd = fs * Q
  // This is a 2-register instruction where one source is implicitly Q
  // Encoding: COP2 + dest + ft=0 + fs + fd + special(011100)
  class VU0_2R_Q<string opstr, bits<6> funct, InstrItinClass Itin>
      : InstSE<(outs VFOpnd:$fd), (ins vu0destmask:$dest, VFOpnd:$fs, VU0QOpnd:$q),
               !strconcat(opstr, "$dest\t$fd, $fs, $q"), [], Itin, FrmR, opstr> {
    bits<5> fd;
    bits<5> fs;
    bits<4> dest;

    let Inst{31-26} = 0b010010;  // COP2
    let Inst{25}    = 1;         // CO bit
    let Inst{24-21} = dest;      // destination mask
    let Inst{20-16} = 0;         // ft = 0 for Q operations
    let Inst{15-11} = fs;
    let Inst{10-6}  = fd;
    let Inst{5-0}   = funct;
    let Uses = [VU0Q];
  }

  def VMULQ  : VU0_2R_Q<"vmulq", 0b011100, II_VU0_VMUL>;
  def VADDQ  : VU0_2R_Q<"vaddq", 0b100000, II_VU0_VADD>;
  def VSUBQ  : VU0_2R_Q<"vsubq", 0b100100, II_VU0_VSUB>;
  def VMADDQ : VU0_2R_Q<"vmaddq", 0b100001, II_VU0_VMADD> {
    let Uses = [VU0Q, VU0ACC];
  }
  def VMSUBQ : VU0_2R_Q<"vmsubq", 0b100101, II_VU0_VMSUB> {
    let Uses = [VU0Q, VU0ACC];
  }

  //===--------------------------------------------------------------------===//
  // VU0 Outer Product Instructions
  //===--------------------------------------------------------------------===//

  // VOPMULA - Outer product multiply-add to ACC
  // ACC = fs x ft (cross product like operation)
  // Encoding: COP2 + dest + ft + fs + 0 + special(0101111110)
  def VOPMULA : InstSE<(outs), (ins vu0destmask:$dest, VU0AccOpnd:$acc, VFOpnd:$fs, VFOpnd:$ft),
                       "vopmula$dest\t$acc, $fs, $ft", [], II_VU0_VOPMULA, FrmR, "vopmula"> {
    bits<5> fs;
    bits<5> ft;
    bits<4> dest;

    let Inst{31-26} = 0b010010;  // COP2
    let Inst{25}    = 1;         // CO bit
    let Inst{24-21} = dest;      // destination mask
    let Inst{20-16} = ft;
    let Inst{15-11} = fs;
    let Inst{10-0}  = 0b00001011110;  // special opcode
    let Defs = [VU0ACC];
  }

  // VOPMSUB - Outer product multiply-subtract
  // fd = ACC - fs x ft (cross product like operation)
  // Encoding: COP2 + dest + ft + fs + fd + special(0101111)
  def VOPMSUB : InstSE<(outs VFOpnd:$fd), (ins vu0destmask:$dest, VFOpnd:$fs, VFOpnd:$ft),
                       "vopmsub$dest\t$fd, $fs, $ft", [], II_VU0_VOPMSUB, FrmR, "vopmsub"> {
    bits<5> fd;
    bits<5> fs;
    bits<5> ft;
    bits<4> dest;

    let Inst{31-26} = 0b010010;  // COP2
    let Inst{25}    = 1;         // CO bit
    let Inst{24-21} = dest;      // destination mask
    let Inst{20-16} = ft;
    let Inst{15-11} = fs;
    let Inst{10-6}  = fd;
    let Inst{5-0}   = 0b101110;  // special opcode
    let Uses = [VU0ACC];
  }

  //===--------------------------------------------------------------------===//
  // VU0 Clip Detection
  //===--------------------------------------------------------------------===//

  // VCLIPW - Clip detection
  // Tests fs.xyz against ft.w and updates clip flags
  // Encoding: COP2 + dest + ft + fs + 0 + special(0011111111)
  def VCLIPW : InstSE<(outs), (ins vu0destmask:$dest, VFOpnd:$fs, VFOpnd:$ft),
                      "vclipw$dest\t$fs, $ft", [], II_VU0_VCLIP, FrmR, "vclipw"> {
    bits<5> fs;
    bits<5> ft;
    bits<4> dest;

    let Inst{31-26} = 0b010010;  // COP2
    let Inst{25}    = 1;         // CO bit
    let Inst{24-21} = dest;      // destination mask
    let Inst{20-16} = ft;
    let Inst{15-11} = fs;
    let Inst{10-0}  = 0b00001111111;  // special opcode
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
  // Uses VFWithFieldOpnd which parses $vf0w and creates (field, register) pair
  let hasCompleteDecoder = 0 in  // Assembly only - no disassembly support yet
  def VDIV : InstSE<(outs VU0QOpnd:$q),
                    (ins VFWithFieldOpnd:$src1, VFWithFieldOpnd:$src2),
                    "vdiv\t$q, $src1, $src2", [], II_VU0_VDIV, FrmR, "vdiv"> {
    bits<7> src1;  // Combined: fsf(2) + fs(5)
    bits<7> src2;  // Combined: ftf(2) + ft(5)

    let Inst{31-26} = 0b010010;  // COP2
    let Inst{25}    = 1;         // CO bit
    let Inst{24-23} = src2{6-5}; // ftf - field selector for ft
    let Inst{22-21} = src1{6-5}; // fsf - field selector for fs
    let Inst{20-16} = src2{4-0}; // ft register
    let Inst{15-11} = src1{4-0}; // fs register
    let Inst{10-0}  = 0b01110111100;  // VDIV opcode
    let Defs = [VU0Q];
    let hasSideEffects = 1;
  }

  // VRSQRT - Reciprocal square root: Q = fs[fsf] / sqrt(ft[ftf])
  // Encoding: 010010 1 ftf(2) fsf(2) ft(5) fs(5) 01110111110
  let hasCompleteDecoder = 0 in  // Assembly only - no disassembly support yet
  def VRSQRT : InstSE<(outs VU0QOpnd:$q),
                      (ins VFWithFieldOpnd:$src1, VFWithFieldOpnd:$src2),
                      "vrsqrt\t$q, $src1, $src2", [], II_VU0_VDIV, FrmR, "vrsqrt"> {
    bits<7> src1;  // Combined: fsf(2) + fs(5)
    bits<7> src2;  // Combined: ftf(2) + ft(5)

    let Inst{31-26} = 0b010010;  // COP2
    let Inst{25}    = 1;         // CO bit
    let Inst{24-23} = src2{6-5}; // ftf - field selector for ft
    let Inst{22-21} = src1{6-5}; // fsf - field selector for fs
    let Inst{20-16} = src2{4-0}; // ft register
    let Inst{15-11} = src1{4-0}; // fs register
    let Inst{10-0}  = 0b01110111110;  // VRSQRT opcode
    let Defs = [VU0Q];
    let hasSideEffects = 1;
  }

  // VSQRT - Square root: Q = sqrt(ft[ftf])
  // Only uses ft register, fs is fixed to 0
  // Encoding: 010010 1 ftf(2) 00 ft(5) 00000 01110111101
  let hasCompleteDecoder = 0 in  // Assembly only - no disassembly support yet
  def VSQRT : InstSE<(outs VU0QOpnd:$q),
                     (ins VFWithFieldOpnd:$src),
                     "vsqrt\t$q, $src", [], II_VU0_VDIV, FrmR, "vsqrt"> {
    bits<7> src;  // Combined: ftf(2) + ft(5)

    let Inst{31-26} = 0b010010;  // COP2
    let Inst{25}    = 1;         // CO bit
    let Inst{24-23} = src{6-5};  // ftf - field selector for ft
    let Inst{22-21} = 0b00;      // fsf = 0 (unused)
    let Inst{20-16} = src{4-0};  // ft register
    let Inst{15-11} = 0b00000;   // fs = 0 (unused)
    let Inst{10-0}  = 0b01110111101;  // VSQRT opcode
    let Defs = [VU0Q];
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
                    "cfc2\t$rt, $rd", [], II_CFC2, FrmFR, "cfc2">,
             MFC2_FM<0b00010> {
    let DecoderNamespace = "R5900";
  }

  // CTC2 - Copy To Coprocessor 2 Control: VU0_CR = GPR
  // Writes to VU0 control register (VI16-VI31) from GPR
  def CTC2 : InstSE<(outs COP2Opnd:$rd), (ins GPR32Opnd:$rt),
                    "ctc2\t$rt, $rd", [], II_CTC2, FrmFR, "ctc2">,
             MFC2_FM<0b00110> {
    let DecoderNamespace = "R5900";
  }

  // QMFC2 - Quadword Move From Coprocessor 2: GPR128 = VF
  // Transfers 128-bit data from VF register to GPR128
  // Encoding: COP2(6) | 00001(5) | rt(5) | rd(5) | 0(11)
  def QMFC2 : InstSE<(outs GPR128Opnd:$rt), (ins VFOpnd:$rd),
                     "qmfc2\t$rt, $rd", [], II_CFC2, FrmFR, "qmfc2">,
              MFC2_FM<0b00001> {
    let DecoderNamespace = "R5900";
  }

  // QMTC2 - Quadword Move To Coprocessor 2: VF = GPR128
  // Transfers 128-bit data from GPR128 to VF register
  // Encoding: COP2(6) | 00101(5) | rt(5) | rd(5) | 0(11)
  def QMTC2 : InstSE<(outs VFOpnd:$rd), (ins GPR128Opnd:$rt),
                     "qmtc2\t$rt, $rd", [], II_CTC2, FrmFR, "qmtc2">,
              MFC2_FM<0b00101> {
    let DecoderNamespace = "R5900";
  }

  // Bitcast patterns between v4f32 (VF register) and v4i32 (GPR128)
  // Used for EXTRACT_VECTOR_ELT and INSERT_VECTOR_ELT lowering
  def : Pat<(v4i32 (bitconvert v4f32:$src)), (QMFC2 v4f32:$src)>;
  def : Pat<(v4f32 (bitconvert v4i32:$src)), (QMTC2 v4i32:$src)>;

} // Predicates = [HasVU0]
