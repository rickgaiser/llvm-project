//===- DVPVUInstrFormats.td - DVPVU Instruction Formats ----*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file describes the PS2 VU instruction formats.
//
// VU Instruction Word (64 bits):
//   Bits 63-32: Upper Instruction (FMAC operations)
//   Bits 31-0:  Lower Instruction (various operations)
//
// Upper Instruction Format (32 bits):
//   Bit 31: I-bit (when set, lower field is immediate for I register)
//   Bits 30-27: Reserved
//   Bits 26-25: 00
//   Bits 24-21: DEST mask (xyzw)
//   Bits 20-16: FT register (5 bits)
//   Bits 15-11: FS register (5 bits)
//   Bits 10-6:  FD register (5 bits)
//   Bits 5-0:   Opcode (varies by instruction)
//
// Lower Instruction Formats vary by instruction type.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// TSFlags - Target-Specific Flags
//===----------------------------------------------------------------------===//
//
// Bit 0: IsUpper - Instruction uses the Upper slot (FMAC)
// Bit 1: IsLower - Instruction uses the Lower slot
//
// These flags are used by the VLIW Packetizer to determine which slot
// an instruction occupies and to bundle Upper+Lower instruction pairs.
//

//===----------------------------------------------------------------------===//
// Base Instruction Class
//===----------------------------------------------------------------------===//

class DVPVUInst<dag outs, dag ins, string asmstr, list<dag> pattern>
    : Instruction {
  let Namespace = "DVPVU";
  let Size = 8;  // 64-bit instruction

  dag OutOperandList = outs;
  dag InOperandList = ins;
  let AsmString = asmstr;
  let Pattern = pattern;

  let DecoderNamespace = "DVPVU";

  // 64-bit instruction encoding
  field bits<64> Inst;
  bits<32> Upper = 0;
  bits<32> Lower = 0;
  let Inst{63-32} = Upper;
  let Inst{31-0} = Lower;

  // TSFlags for VLIW slot identification
  // Bit 0: IsUpper (uses Upper slot)
  // Bit 1: IsLower (uses Lower slot)
  bit IsUpper = 0;
  bit IsLower = 0;
  let TSFlags{0} = IsUpper;
  let TSFlags{1} = IsLower;
}

//===----------------------------------------------------------------------===//
// Upper Instruction Formats (FMAC operations)
//===----------------------------------------------------------------------===//

// Standard 3-operand upper instruction
class UpperInst<bits<6> opcode, dag outs, dag ins, string asmstr,
                list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<5> vd;
  bits<5> vs;
  bits<5> vt;
  bits<4> dest;

  bits<32> Upper;
  let Upper{31} = 0;      // I-bit
  let Upper{30-27} = 0;   // Reserved
  let Upper{26-25} = 0;
  let Upper{24-21} = dest;
  let Upper{20-16} = vt;
  let Upper{15-11} = vs;
  let Upper{10-6} = vd;
  let Upper{5-0} = opcode;

  // Default lower to NOP
  bits<32> Lower = 0b10000000000000000000000000110011;

  let IsUpper = 1;
}

// Upper NOP instruction (no operands)
class UpperNopInst<bits<6> opcode, dag outs, dag ins, string asmstr,
                   list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<32> Upper;
  let Upper{31} = 0;
  let Upper{30-27} = 0;
  let Upper{26-25} = 0;
  let Upper{24-21} = 0;  // DEST = 0
  let Upper{20-16} = 0;  // VT = 0
  let Upper{15-11} = 0;  // VS = 0
  let Upper{10-6} = 0;   // VD = 0
  let Upper{5-0} = opcode;

  // Default lower to NOP
  bits<32> Lower = 0b10000000000000000000000000110011;

  let IsUpper = 1;
}

// 2-operand upper instruction (e.g., ABS)
class UpperInst2Op<bits<6> opcode, dag outs, dag ins, string asmstr,
                   list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<5> vd;
  bits<5> vs;
  bits<4> dest;

  bits<32> Upper;
  let Upper{31} = 0;
  let Upper{30-27} = 0;
  let Upper{26-25} = 0;
  let Upper{24-21} = dest;
  let Upper{20-16} = 0;
  let Upper{15-11} = vs;
  let Upper{10-6} = vd;
  let Upper{5-0} = opcode;

  bits<32> Lower = 0b10000000000000000000000000110011;

  let IsUpper = 1;
}

//===----------------------------------------------------------------------===//
// Lower Instruction Formats
//===----------------------------------------------------------------------===//

// Standard 3-register lower instruction (e.g., IADD)
class LowerInst<bits<7> opcode, dag outs, dag ins, string asmstr,
                list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<4> id;
  bits<4> is;
  bits<4> it;

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-21} = 0;    // DEST (not used for integer ops)
  let Lower{20-16} = {0, it};
  let Lower{15-11} = {0, is};
  let Lower{10-6} = {0, id};
  let Lower{5-0} = 0;

  // Default upper to NOP
  bits<32> Upper = 0b00101111000000000000000000000000;

  let IsLower = 1;
}

// Lower instruction with immediate (e.g., IADDI)
class LowerImmInst<bits<7> opcode, dag outs, dag ins, string asmstr,
                   list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<4> it;
  bits<4> is;
  bits<5> imm;

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-21} = 0;
  let Lower{20-16} = {0, it};
  let Lower{15-11} = {0, is};
  let Lower{10-6} = imm;
  let Lower{5-0} = 0;

  bits<32> Upper = 0b00101111000000000000000000000000;

  let IsLower = 1;
}

// Load/Store memory instruction
class LowerMemInst<bits<7> opcode, dag outs, dag ins, string asmstr,
                   list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<5> vt;
  bits<4> is;
  bits<11> offset;
  bits<4> dest;

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-21} = dest;
  let Lower{20-16} = vt;
  let Lower{15-11} = {0, is};
  let Lower{10-0} = offset;

  bits<32> Upper = 0b00101111000000000000000000000000;

  let IsLower = 1;
}

// Load/Store with increment
class LowerMemIncInst<bits<7> opcode, dag outs, dag ins, string asmstr,
                      list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<5> vt;
  bits<4> is;
  bits<4> dest;

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-21} = dest;
  let Lower{20-16} = vt;
  let Lower{15-11} = {0, is};
  let Lower{10-0} = 0;

  bits<32> Upper = 0b00101111000000000000000000000000;

  let IsLower = 1;
}

// Integer word memory instruction
class LowerWordMemInst<bits<7> opcode, dag outs, dag ins, string asmstr,
                       list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<4> it;
  bits<4> is;
  bits<11> offset;
  bits<2> f;  // Field selector

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-23} = f;
  let Lower{22-21} = 0;
  let Lower{20-16} = {0, it};
  let Lower{15-11} = {0, is};
  let Lower{10-0} = offset;

  bits<32> Upper = 0b00101111000000000000000000000000;

  let IsLower = 1;
}

// Division instruction
class LowerDivInst<bits<7> opcode, dag outs, dag ins, string asmstr,
                   list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<5> vs;
  bits<5> vt;
  bits<2> fsf;  // Source field
  bits<2> ftf;  // Target field

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-23} = ftf;
  let Lower{22-21} = fsf;
  let Lower{20-16} = vt;
  let Lower{15-11} = vs;
  let Lower{10-0} = 0;

  bits<32> Upper = 0b00101111000000000000000000000000;

  let IsLower = 1;
}

// SQRT instruction
class LowerSqrtInst<bits<7> opcode, dag outs, dag ins, string asmstr,
                    list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<5> vt;
  bits<2> ftf;

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-23} = ftf;
  let Lower{22-16} = 0;
  let Lower{15-11} = vt;
  let Lower{10-0} = 0;

  bits<32> Upper = 0b00101111000000000000000000000000;

  let IsLower = 1;
}

// RSQRT instruction
class LowerRsqrtInst<bits<7> opcode, dag outs, dag ins, string asmstr,
                     list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<5> vs;
  bits<5> vt;
  bits<2> fsf;
  bits<2> ftf;

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-23} = ftf;
  let Lower{22-21} = fsf;
  let Lower{20-16} = vt;
  let Lower{15-11} = vs;
  let Lower{10-0} = 0;

  bits<32> Upper = 0b00101111000000000000000000000000;

  let IsLower = 1;
}

// Wait instruction
class LowerWaitInst<bits<7> opcode, dag outs, dag ins, string asmstr,
                    list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-0} = 0;

  bits<32> Upper = 0b00101111000000000000000000000000;

  let IsLower = 1;
}

// Unconditional branch
class LowerBranchInst<bits<7> opcode, dag outs, dag ins, string asmstr,
                      list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<11> offset;

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-11} = 0;
  let Lower{10-0} = offset;

  bits<32> Upper = 0b00101111000000000000000000000000;

  let isBranch = 1;
  let isTerminator = 1;
  let IsLower = 1;
}

// Branch and link
class LowerBranchLinkInst<bits<7> opcode, dag outs, dag ins, string asmstr,
                          list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<4> it;
  bits<11> offset;

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-21} = 0;
  let Lower{20-16} = {0, it};
  let Lower{15-11} = 0;
  let Lower{10-0} = offset;

  bits<32> Upper = 0b00101111000000000000000000000000;

  let isBranch = 1;
  let isCall = 1;
  let IsLower = 1;
}

// Jump register
class LowerJumpRegInst<bits<7> opcode, dag outs, dag ins, string asmstr,
                       list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<4> is;

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-16} = 0;
  let Lower{15-11} = {0, is};
  let Lower{10-0} = 0;

  bits<32> Upper = 0b00101111000000000000000000000000;

  let isBranch = 1;
  let isTerminator = 1;
  let isBarrier = 1;
  let isIndirectBranch = 1;
  let IsLower = 1;
}

// Jump and link register
class LowerJumpLinkRegInst<bits<7> opcode, dag outs, dag ins, string asmstr,
                           list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<4> it;
  bits<4> is;

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-21} = 0;
  let Lower{20-16} = {0, it};
  let Lower{15-11} = {0, is};
  let Lower{10-0} = 0;

  bits<32> Upper = 0b00101111000000000000000000000000;

  let isCall = 1;
  let IsLower = 1;
}

// Conditional branch with two registers
class LowerCondBranchInst<bits<7> opcode, dag outs, dag ins, string asmstr,
                          list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<4> is;
  bits<4> it;
  bits<11> offset;

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-21} = 0;
  let Lower{20-16} = {0, it};
  let Lower{15-11} = {0, is};
  let Lower{10-0} = offset;

  bits<32> Upper = 0b00101111000000000000000000000000;

  let isBranch = 1;
  let isTerminator = 1;
  let IsLower = 1;
}

// Conditional branch with single register
class LowerCondBranchSingleInst<bits<7> opcode, dag outs, dag ins, string asmstr,
                                list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<4> is;
  bits<11> offset;

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-16} = 0;
  let Lower{15-11} = {0, is};
  let Lower{10-0} = offset;

  bits<32> Upper = 0b00101111000000000000000000000000;

  let isBranch = 1;
  let isTerminator = 1;
  let IsLower = 1;
}

// NOP instruction
class LowerNopInst<bits<7> opcode, dag outs, dag ins, string asmstr,
                   list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-0} = 0;

  bits<32> Upper = 0b00101111000000000000000000000000;

  let IsLower = 1;
}

// Move instruction
class LowerMoveInst<bits<7> opcode, dag outs, dag ins, string asmstr,
                    list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<5> vt;
  bits<5> vs;
  bits<4> dest;

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-21} = dest;
  let Lower{20-16} = vt;
  let Lower{15-11} = vs;
  let Lower{10-0} = 0b00000110000;

  bits<32> Upper = 0b00101111000000000000000000000000;

  let IsLower = 1;
}

// MR32 (rotate) instruction
class LowerMr32Inst<bits<7> opcode, dag outs, dag ins, string asmstr,
                    list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<5> vt;
  bits<5> vs;
  bits<4> dest;

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-21} = dest;
  let Lower{20-16} = vt;
  let Lower{15-11} = vs;
  let Lower{10-0} = 0b00000110001;

  bits<32> Upper = 0b00101111000000000000000000000000;

  let IsLower = 1;
}

// Move from integer register
class LowerMfirInst<bits<7> opcode, dag outs, dag ins, string asmstr,
                    list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<5> vt;
  bits<4> is;
  bits<4> dest;

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-21} = dest;
  let Lower{20-16} = vt;
  let Lower{15-11} = {0, is};
  let Lower{10-0} = 0b00000110011;

  bits<32> Upper = 0b00101111000000000000000000000000;

  let IsLower = 1;
}

// Move to integer register
class LowerMtirInst<bits<7> opcode, dag outs, dag ins, string asmstr,
                    list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<4> it;
  bits<5> vs;
  bits<2> fsf;

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-23} = fsf;
  let Lower{22-21} = 0;
  let Lower{20-16} = {0, it};
  let Lower{15-11} = vs;
  let Lower{10-0} = 0b00000110100;

  bits<32> Upper = 0b00101111000000000000000000000000;

  let IsLower = 1;
}

// EFU instruction - function code differentiates EFU operations
class LowerEfuInst<bits<7> opcode, bits<6> func, dag outs, dag ins, string asmstr,
                   list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<5> vs;
  bits<2> fsf;

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-23} = fsf;
  let Lower{22-16} = 0;
  let Lower{15-11} = vs;
  let Lower{10-6} = 0;
  let Lower{5-0} = func;

  bits<32> Upper = 0b00101111000000000000000000000000;

  let IsLower = 1;
}

//===----------------------------------------------------------------------===//
// Broadcast Upper Instruction Format
//===----------------------------------------------------------------------===//

// Upper instruction with broadcast (e.g., MULx, ADDy, etc.)
// The BC field (bits 1-0) selects which component of VT to broadcast.
// Assembly syntax: mulx.xyzw vf1, vf2, vf3x
// The opcode is encoded in a 4-bit field (bits 5-2)
class UpperBroadcastInst<bits<4> opcode, bits<2> bc, dag outs, dag ins,
                         string asmstr, list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<5> vd;
  bits<5> vs;
  bits<5> vt;
  bits<4> dest;

  bits<32> Upper;
  let Upper{31} = 0;      // I-bit
  let Upper{30-27} = 0;   // Reserved
  let Upper{26-25} = 0;
  let Upper{24-21} = dest;
  let Upper{20-16} = vt;
  let Upper{15-11} = vs;
  let Upper{10-6} = vd;
  let Upper{5-2} = opcode;
  let Upper{1-0} = bc;

  // Default lower to NOP
  bits<32> Lower = 0b10000000000000000000000000110011;

  let IsUpper = 1;
}

// Upper broadcast instruction for ISel (fixed destmask=0xF)
class UpperBroadcastInstISel<bits<4> opcode, bits<2> bc, dag outs, dag ins,
                             string asmstr, list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<5> vd;
  bits<5> vs;
  bits<5> vt;

  bits<32> Upper;
  let Upper{31} = 0;      // I-bit
  let Upper{30-27} = 0;   // Reserved
  let Upper{26-25} = 0;
  let Upper{24-21} = 0b1111;  // Fixed: dest = xyzw
  let Upper{20-16} = vt;
  let Upper{15-11} = vs;
  let Upper{10-6} = vd;
  let Upper{5-2} = opcode;
  let Upper{1-0} = bc;

  // Default lower to NOP
  bits<32> Lower = 0b10000000000000000000000000110011;

  let isCodeGenOnly = 1;
  let IsUpper = 1;
}

//===----------------------------------------------------------------------===//
// ISel Instruction Formats (CodeGen only, fixed destmask)
//===----------------------------------------------------------------------===//

// Upper instruction with fixed destmask=0xF (.xyzw) for ISel
// No destmask operand - pattern matching doesn't need masks
class UpperInstISel<bits<6> opcode, dag outs, dag ins, string asmstr,
                    list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<5> vd;
  bits<5> vs;
  bits<5> vt;

  bits<32> Upper;
  let Upper{31} = 0;      // I-bit
  let Upper{30-27} = 0;   // Reserved
  let Upper{26-25} = 0;
  let Upper{24-21} = 0b1111;  // Fixed: dest = xyzw
  let Upper{20-16} = vt;
  let Upper{15-11} = vs;
  let Upper{10-6} = vd;
  let Upper{5-0} = opcode;

  // Default lower to NOP
  bits<32> Lower = 0b10000000000000000000000000110011;

  let isCodeGenOnly = 1;
  let IsUpper = 1;
}

// Lower memory instruction with fixed destmask=0xF (.xyzw) for ISel
class LowerMemInstISel<bits<7> opcode, dag outs, dag ins, string asmstr,
                       list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<5> vt;
  bits<4> is;
  bits<11> offset;

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-21} = 0b1111;  // Fixed: dest = xyzw
  let Lower{20-16} = vt;
  let Lower{15-11} = {0, is};
  let Lower{10-0} = offset;

  // Default upper to NOP
  bits<32> Upper = 0b00101111000000000000000000000000;

  let isCodeGenOnly = 1;
  let IsLower = 1;
}

// Lower integer word memory instruction with fixed field=x for ISel
class LowerWordMemInstISel<bits<7> opcode, dag outs, dag ins, string asmstr,
                           list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  bits<4> it;
  bits<4> is;
  bits<11> offset;

  bits<32> Lower;
  let Lower{31-25} = opcode;
  let Lower{24-23} = 0b00;  // Fixed: field = x
  let Lower{22-21} = 0;
  let Lower{20-16} = {0, it};
  let Lower{15-11} = {0, is};
  let Lower{10-0} = offset;

  // Default upper to NOP
  bits<32> Upper = 0b00101111000000000000000000000000;

  let isCodeGenOnly = 1;
  let IsLower = 1;
}

//===----------------------------------------------------------------------===//
// Pseudo Instructions
//===----------------------------------------------------------------------===//

class Pseudo<dag outs, dag ins, string asmstr, list<dag> pattern>
    : DVPVUInst<outs, ins, asmstr, pattern> {
  let isPseudo = 1;
  let isCodeGenOnly = 1;
}
