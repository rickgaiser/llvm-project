//===- MipsR5900InstrFormats.td - R5900 Instruction Formats -*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file describes R5900 EE Core and MMI instruction formats.
//
//===----------------------------------------------------------------------===//

// R5900 base instruction class
class R5900Inst<string opstr = "">
    : MipsInst<(outs), (ins), "", [], NoItinerary, FrmOther> {
  let ASEPredicate = [HasR5900];
  string BaseOpcode = opstr;
}

class PseudoR5900<dag outs, dag ins, list<dag> pattern,
                  InstrItinClass itin = IIPseudo>
    : MipsPseudo<outs, ins, pattern, itin> {
  let ASEPredicate = [HasR5900];
}

//===----------------------------------------------------------------------===//
//  R5900 Enable/disable interrupt instruction format <COP0|CO|0|funct>
//  EI: 0x42000038, DI: 0x42000039
//===----------------------------------------------------------------------===//

class EI_FM_R5900<bits<6> funct> : StdArch
{
  bits<32> Inst;
  let Inst{31-26} = 0x10;  // COP0
  let Inst{25}    = 1;     // CO bit
  let Inst{24-6}  = 0;
  let Inst{5-0}   = funct;
}

//===----------------------------------------------------------------------===//
//  R5900 Breakpoint register move instruction formats
//  MF*: COP0 rs=0, MT*: COP0 rs=4, with rd=24 and function code
//  mfbpc $rt = 0x4002c000 | (rt << 16)
//  mtbpc $rt = 0x4084c000 | (rt << 16)
//===----------------------------------------------------------------------===//

class MFBP_FM_R5900<bits<3> funct> : StdArch {
  bits<5> rt;
  bits<32> Inst;
  let Inst{31-26} = 0x10;  // COP0
  let Inst{25-21} = 0;     // MF
  let Inst{20-16} = rt;
  let Inst{15-11} = 0x18;  // rd = 24
  let Inst{10-3}  = 0;
  let Inst{2-0}   = funct;
}

class MTBP_FM_R5900<bits<3> funct> : StdArch {
  bits<5> rt;
  bits<32> Inst;
  let Inst{31-26} = 0x10;  // COP0
  let Inst{25-21} = 4;     // MT
  let Inst{20-16} = rt;
  let Inst{15-11} = 0x18;  // rd = 24
  let Inst{10-3}  = 0;
  let Inst{2-0}   = funct;
}

//===----------------------------------------------------------------------===//
//  R5900 COP0 Branch instruction format
//  BC0F: 0x41000000 | offset, BC0T: 0x41010000 | offset
//===----------------------------------------------------------------------===//

class BC0F_FM_R5900<bit tf> : StdArch {
  bits<16> offset;
  bits<32> Inst;
  let Inst{31-26} = 0x10;  // COP0
  let Inst{25-21} = 0x8;   // BC
  let Inst{20-17} = 0;
  let Inst{16}    = tf;
  let Inst{15-0}  = offset;
}

//===----------------------------------------------------------------------===//
// R5900 MMI Instruction Formats
//===----------------------------------------------------------------------===//

// R5900 3-operand multiply format (SPECIAL opcode with rd field)
// Standard MULT uses rd=0, R5900 allows rd to receive low result
class R5900_MULT3_FM<bits<6> funct> : StdArch {
  bits<5> rd;
  bits<5> rs;
  bits<5> rt;

  bits<32> Inst;

  let Inst{31-26} = 0;        // SPECIAL opcode
  let Inst{25-21} = rs;
  let Inst{20-16} = rt;
  let Inst{15-11} = rd;
  let Inst{10-6}  = 0;
  let Inst{5-0}   = funct;
}

// R5900 MMI format for MFHI1/MFLO1 (rd only)
class R5900_MFHILO1_FM<bits<6> funct> : StdArch {
  bits<5> rd;

  bits<32> Inst;

  let Inst{31-26} = 0b011100; // MMI opcode
  let Inst{25-16} = 0;        // rs, rt = 0
  let Inst{15-11} = rd;
  let Inst{10-6}  = 0;
  let Inst{5-0}   = funct;
}

// R5900 MMI format for MTHI1/MTLO1 (rs only)
class R5900_MTHILO1_FM<bits<6> funct> : StdArch {
  bits<5> rs;

  bits<32> Inst;

  let Inst{31-26} = 0b011100; // MMI opcode
  let Inst{25-21} = rs;
  let Inst{20-6}  = 0;        // rt, rd, sa = 0
  let Inst{5-0}   = funct;
}

// R5900 MMI format with rd field for MADD/MULT1 etc
class R5900_MMI_3R_FM<bits<6> funct> : StdArch {
  bits<5> rd;
  bits<5> rs;
  bits<5> rt;

  bits<32> Inst;

  let Inst{31-26} = 0b011100; // MMI opcode
  let Inst{25-21} = rs;
  let Inst{20-16} = rt;
  let Inst{15-11} = rd;
  let Inst{10-6}  = 0;
  let Inst{5-0}   = funct;
}

// R5900 MMI format for 2-operand forms (rd=0)
class R5900_MMI_2R_FM<bits<6> funct> : StdArch {
  bits<5> rs;
  bits<5> rt;

  bits<32> Inst;

  let Inst{31-26} = 0b011100; // MMI opcode
  let Inst{25-21} = rs;
  let Inst{20-16} = rt;
  let Inst{15-6}  = 0;        // rd, sa = 0
  let Inst{5-0}   = funct;
}

// R5900 MMI format for PLZCW: rd = LZC(rs) - 1
// | 31-26  | 25-21 | 20-16 | 15-11 | 10-6 | 5-0    |
// | MMI    | rs    | 0     | rd    | 0    | PLZCW  |
// | 011100 | rs    | 00000 | rd    | 00000| 000100 |
class R5900_MMI_PLZCW_FM : StdArch {
  bits<5> rd;
  bits<5> rs;

  bits<32> Inst;

  let Inst{31-26} = 0b011100; // MMI opcode
  let Inst{25-21} = rs;
  let Inst{20-16} = 0;        // rt = 0
  let Inst{15-11} = rd;
  let Inst{10-6}  = 0;        // sa = 0
  let Inst{5-0}   = 0b000100; // PLZCW function code
}

// R5900 MMI PMFHL format: rd = HI/LO combined
// | 31-26  | 25-16 | 15-11 | 10-6 | 5-0    |
// | MMI    | 0     | rd    | fmt  | PMFHL  |
// | 011100 | 0     | rd    | fmt  | 110000 |
class R5900_MMI_PMFHL_FM<bits<5> fmt> : StdArch {
  bits<5> rd;

  bits<32> Inst;

  let Inst{31-26} = 0b011100; // MMI opcode
  let Inst{25-16} = 0;
  let Inst{15-11} = rd;
  let Inst{10-6}  = fmt;
  let Inst{5-0}   = 0b110000; // PMFHL function code
}

// R5900 MMI PMTHL format: HI/LO = rs (split word units)
// | 31-26  | 25-21 | 20-11 | 10-6 | 5-0    |
// | MMI    | rs    | 0     | fmt  | PMTHL  |
// | 011100 | rs    | 0     | fmt  | 110001 |
class R5900_MMI_PMTHL_FM<bits<5> fmt> : StdArch {
  bits<5> rs;

  bits<32> Inst;

  let Inst{31-26} = 0b011100; // MMI opcode
  let Inst{25-21} = rs;
  let Inst{20-11} = 0;
  let Inst{10-6}  = fmt;
  let Inst{5-0}   = 0b110001; // PMTHL function code
}

// R5900 MMI2 format: 3-register parallel multiply instructions
// | 31-26  | 25-21 | 20-16 | 15-11 | 10-6  | 5-0    |
// | MMI    | rs    | rt    | rd    | subfn | MMI2   |
// | 011100 | rs    | rt    | rd    | subfn | 001001 |
// Used for: PMULTW, PMADDW, PMSUBW, PMULTUW, PMADDUW, etc.
// Note: Parameter named 'subfn' to avoid conflict with 6-bit 'funct' field
// in other MIPS formats which would cause encoding errors.
class R5900_MMI2_FM<bits<5> subfn> : StdArch {
  bits<5> rd;
  bits<5> rs;
  bits<5> rt;

  bits<32> Inst;

  let Inst{31-26} = 0b011100; // MMI opcode
  let Inst{25-21} = rs;
  let Inst{20-16} = rt;
  let Inst{15-11} = rd;
  let Inst{10-6}  = subfn;
  let Inst{5-0}   = 0b001001; // MMI2 function code
}

// R5900 MMI3 format: 3-register parallel multiply instructions
// | 31-26  | 25-21 | 20-16 | 15-11 | 10-6  | 5-0    |
// | MMI    | rs    | rt    | rd    | subfn | MMI3   |
// | 011100 | rs    | rt    | rd    | subfn | 101001 |
// Used for: PMULTUW, PMADDUW, etc.
// Note: Parameter named 'subfn' to avoid conflict with 6-bit 'funct' field
// in other MIPS formats which would cause encoding errors.
class R5900_MMI3_FM<bits<5> subfn> : StdArch {
  bits<5> rd;
  bits<5> rs;
  bits<5> rt;

  bits<32> Inst;

  let Inst{31-26} = 0b011100; // MMI opcode
  let Inst{25-21} = rs;
  let Inst{20-16} = rt;
  let Inst{15-11} = rd;
  let Inst{10-6}  = subfn;
  let Inst{5-0}   = 0b101001; // MMI3 function code
}

// R5900 MMI0 format: 3-register parallel instructions
// | 31-26  | 25-21 | 20-16 | 15-11 | 10-6  | 5-0    |
// | MMI    | rs    | rt    | rd    | subfn | MMI0   |
// | 011100 | rs    | rt    | rd    | subfn | 001000 |
// Used for: PMAXW, PMAXH, etc.
// Note: Parameter named 'subfn' to avoid conflict with 6-bit 'funct' field
// in other MIPS formats which would cause encoding errors.
class R5900_MMI0_FM<bits<5> subfn> : StdArch {
  bits<5> rd;
  bits<5> rs;
  bits<5> rt;

  bits<32> Inst;

  let Inst{31-26} = 0b011100; // MMI opcode
  let Inst{25-21} = rs;
  let Inst{20-16} = rt;
  let Inst{15-11} = rd;
  let Inst{10-6}  = subfn;
  let Inst{5-0}   = 0b001000; // MMI0 function code
}

// R5900 MMI1 format: 3-register parallel instructions
// | 31-26  | 25-21 | 20-16 | 15-11 | 10-6  | 5-0    |
// | MMI    | rs    | rt    | rd    | subfn | MMI1   |
// | 011100 | rs    | rt    | rd    | subfn | 101000 |
// Used for: PMINW, PMINH, etc.
// Note: Parameter named 'subfn' to avoid conflict with 6-bit 'funct' field
// in other MIPS formats which would cause encoding errors.
class R5900_MMI1_FM<bits<5> subfn> : StdArch {
  bits<5> rd;
  bits<5> rs;
  bits<5> rt;

  bits<32> Inst;

  let Inst{31-26} = 0b011100; // MMI opcode
  let Inst{25-21} = rs;
  let Inst{20-16} = rt;
  let Inst{15-11} = rd;
  let Inst{10-6}  = subfn;
  let Inst{5-0}   = 0b101000; // MMI1 function code
}

// R5900 MMI0 2-operand format: for PABSW (rd, rt with rs=0)
// | 31-26  | 25-21 | 20-16 | 15-11 | 10-6  | 5-0    |
// | MMI    | 0     | rt    | rd    | subfn | MMI0   |
// | 011100 | 00000 | rt    | rd    | subfn | 001000 |
class R5900_MMI0_2R_FM<bits<5> subfn> : StdArch {
  bits<5> rd;
  bits<5> rt;

  bits<32> Inst;

  let Inst{31-26} = 0b011100; // MMI opcode
  let Inst{25-21} = 0;        // rs = 0
  let Inst{20-16} = rt;
  let Inst{15-11} = rd;
  let Inst{10-6}  = subfn;
  let Inst{5-0}   = 0b001000; // MMI0 function code
}

// R5900 MMI1 2-operand format: for PABSH (rd, rt with rs=0)
// | 31-26  | 25-21 | 20-16 | 15-11 | 10-6  | 5-0    |
// | MMI    | 0     | rt    | rd    | subfn | MMI1   |
// | 011100 | 00000 | rt    | rd    | subfn | 101000 |
class R5900_MMI1_2R_FM<bits<5> subfn> : StdArch {
  bits<5> rd;
  bits<5> rt;

  bits<32> Inst;

  let Inst{31-26} = 0b011100; // MMI opcode
  let Inst{25-21} = 0;        // rs = 0
  let Inst{20-16} = rt;
  let Inst{15-11} = rd;
  let Inst{10-6}  = subfn;
  let Inst{5-0}   = 0b101000; // MMI1 function code
}

// R5900 MMI2 2-operand format: for PEXEH, PEXEW, PREVH, PROT3W (rd, rt with rs=0)
// | 31-26  | 25-21 | 20-16 | 15-11 | 10-6  | 5-0    |
// | MMI    | 0     | rt    | rd    | subfn | MMI2   |
// | 011100 | 00000 | rt    | rd    | subfn | 001001 |
class R5900_MMI2_2R_FM<bits<5> subfn> : StdArch {
  bits<5> rd;
  bits<5> rt;

  bits<32> Inst;

  let Inst{31-26} = 0b011100; // MMI opcode
  let Inst{25-21} = 0;        // rs = 0
  let Inst{20-16} = rt;
  let Inst{15-11} = rd;
  let Inst{10-6}  = subfn;
  let Inst{5-0}   = 0b001001; // MMI2 function code
}

// R5900 MMI3 2-operand format: for PCPYH, PEXCH, PEXCW (rd, rt with rs=0)
// | 31-26  | 25-21 | 20-16 | 15-11 | 10-6  | 5-0    |
// | MMI    | 0     | rt    | rd    | subfn | MMI3   |
// | 011100 | 00000 | rt    | rd    | subfn | 101001 |
class R5900_MMI3_2R_FM<bits<5> subfn> : StdArch {
  bits<5> rd;
  bits<5> rt;

  bits<32> Inst;

  let Inst{31-26} = 0b011100; // MMI opcode
  let Inst{25-21} = 0;        // rs = 0
  let Inst{20-16} = rt;
  let Inst{15-11} = rd;
  let Inst{10-6}  = subfn;
  let Inst{5-0}   = 0b101001; // MMI3 function code
}

// R5900 MMI2 divide format: for PDIVW, PDIVBW (rs, rt only, rd=0)
// | 31-26  | 25-21 | 20-16 | 15-11 | 10-6  | 5-0    |
// | MMI    | rs    | rt    | 0     | subfn | MMI2   |
// | 011100 | rs    | rt    | 00000 | subfn | 001001 |
class R5900_MMI2_DIV_FM<bits<5> subfn> : StdArch {
  bits<5> rs;
  bits<5> rt;

  bits<32> Inst;

  let Inst{31-26} = 0b011100; // MMI opcode
  let Inst{25-21} = rs;
  let Inst{20-16} = rt;
  let Inst{15-11} = 0;        // rd = 0
  let Inst{10-6}  = subfn;
  let Inst{5-0}   = 0b001001; // MMI2 function code
}

// R5900 MMI3 divide format: for PDIVUW (rs, rt only, rd=0)
// | 31-26  | 25-21 | 20-16 | 15-11 | 10-6  | 5-0    |
// | MMI    | rs    | rt    | 0     | subfn | MMI3   |
// | 011100 | rs    | rt    | 00000 | subfn | 101001 |
class R5900_MMI3_DIV_FM<bits<5> subfn> : StdArch {
  bits<5> rs;
  bits<5> rt;

  bits<32> Inst;

  let Inst{31-26} = 0b011100; // MMI opcode
  let Inst{25-21} = rs;
  let Inst{20-16} = rt;
  let Inst{15-11} = 0;        // rd = 0
  let Inst{10-6}  = subfn;
  let Inst{5-0}   = 0b101001; // MMI3 function code
}

// R5900 MMI2 rd-only format: for PMFHI/PMFLO
// | 31-26  | 25-16 | 15-11 | 10-6  | 5-0    |
// | MMI    | 0     | rd    | subfn | MMI2   |
// | 011100 | 0     | rd    | subfn | 001001 |
class R5900_MMI2_RD_FM<bits<5> subfn> : StdArch {
  bits<5> rd;

  bits<32> Inst;

  let Inst{31-26} = 0b011100; // MMI opcode
  let Inst{25-16} = 0;        // rs, rt = 0
  let Inst{15-11} = rd;
  let Inst{10-6}  = subfn;
  let Inst{5-0}   = 0b001001; // MMI2 function code
}

// R5900 MMI3 rs-only format: for PMTHI/PMTLO
// | 31-26  | 25-21 | 20-11 | 10-6  | 5-0    |
// | MMI    | rs    | 0     | subfn | MMI3   |
// | 011100 | rs    | 0     | subfn | 101001 |
class R5900_MMI3_RS_FM<bits<5> subfn> : StdArch {
  bits<5> rs;

  bits<32> Inst;

  let Inst{31-26} = 0b011100; // MMI opcode
  let Inst{25-21} = rs;
  let Inst{20-11} = 0;        // rt, rd = 0
  let Inst{10-6}  = subfn;
  let Inst{5-0}   = 0b101001; // MMI3 function code
}

// R5900 MMI Shift format: for PSLLH/PSRLH/PSRAH/PSLLW/PSRLW/PSRAW
// | 31-26  | 25-21 | 20-16 | 15-11 | 10-6 | 5-0    |
// | MMI    | 0     | rt    | rd    | sa   | funct  |
// | 011100 | 00000 | rt    | rd    | sa   | funct  |
class R5900_MMI_SHIFT_FM<bits<6> funct> : StdArch {
  bits<5> rd;
  bits<5> rt;
  bits<5> sa;

  bits<32> Inst;

  let Inst{31-26} = 0b011100; // MMI opcode
  let Inst{25-21} = 0;        // rs = 0
  let Inst{20-16} = rt;
  let Inst{15-11} = rd;
  let Inst{10-6}  = sa;       // shift amount
  let Inst{5-0}   = funct;
}

// R5900 MFSA format: SPECIAL class, rd only
// | 31-26   | 25-16 | 15-11 | 10-6  | 5-0    |
// | SPECIAL | 0     | rd    | 0     | MFSA   |
// | 000000  | 0     | rd    | 00000 | 101000 |
class R5900_MFSA_FM : StdArch {
  bits<5> rd;

  bits<32> Inst;

  let Inst{31-26} = 0;        // SPECIAL
  let Inst{25-16} = 0;        // rs, rt = 0
  let Inst{15-11} = rd;
  let Inst{10-6}  = 0;        // sa = 0
  let Inst{5-0}   = 0b101000; // MFSA function
}

// R5900 MTSA format: SPECIAL class, rs only
// | 31-26   | 25-21 | 20-6   | 5-0    |
// | SPECIAL | rs    | 0      | MTSA   |
// | 000000  | rs    | 0      | 101001 |
class R5900_MTSA_FM : StdArch {
  bits<5> rs;

  bits<32> Inst;

  let Inst{31-26} = 0;        // SPECIAL
  let Inst{25-21} = rs;
  let Inst{20-6}  = 0;        // rt, rd, sa = 0
  let Inst{5-0}   = 0b101001; // MTSA function
}

// R5900 MTSAB format: REGIMM class
// | 31-26  | 25-21 | 20-16 | 15-0      |
// | REGIMM | rs    | MTSAB | immediate |
// | 000001 | rs    | 11000 | imm       |
class R5900_MTSAB_FM : StdArch {
  bits<5> rs;
  bits<16> imm;

  bits<32> Inst;

  let Inst{31-26} = 0b000001; // REGIMM
  let Inst{25-21} = rs;
  let Inst{20-16} = 0b11000;  // MTSAB function
  let Inst{15-0}  = imm;
}

// R5900 MTSAH format: REGIMM class
// | 31-26  | 25-21 | 20-16 | 15-0      |
// | REGIMM | rs    | MTSAH | immediate |
// | 000001 | rs    | 11001 | imm       |
class R5900_MTSAH_FM : StdArch {
  bits<5> rs;
  bits<16> imm;

  bits<32> Inst;

  let Inst{31-26} = 0b000001; // REGIMM
  let Inst{25-21} = rs;
  let Inst{20-16} = 0b11001;  // MTSAH function
  let Inst{15-0}  = imm;
}
