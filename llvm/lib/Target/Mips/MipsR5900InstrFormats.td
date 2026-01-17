//===- MipsR5900InstrFormats.td - R5900 Instruction Formats -*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file describes R5900 EE Core instruction formats.
//
//===----------------------------------------------------------------------===//

// R5900 base instruction class
class R5900Inst<string opstr = "">
    : MipsInst<(outs), (ins), "", [], NoItinerary, FrmOther> {
  let ASEPredicate = [HasR5900];
  string BaseOpcode = opstr;
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
// R5900 3-operand multiply format (SPECIAL opcode with rd field)
// Standard MULT uses rd=0, R5900 allows rd to receive low result
//===----------------------------------------------------------------------===//

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

//===----------------------------------------------------------------------===//
// R5900 MMI format with rd field for MADD/MADDU
// | 31-26  | 25-21 | 20-16 | 15-11 | 10-6 | 5-0   |
// | MMI    | rs    | rt    | rd    | 0    | funct |
// | 011100 | rs    | rt    | rd    | 00000| funct |
//===----------------------------------------------------------------------===//

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
