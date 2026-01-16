//===- MipsVU0InstrFormats.td - VU0 Instruction Formats ----*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file describes R5900 VU0 (COP2) macro mode instruction formats.
//
//===----------------------------------------------------------------------===//

// VU0 base instruction class
class VU0Inst<string opstr = "">
    : MipsInst<(outs), (ins), "", [], NoItinerary, FrmOther> {
  let ASEPredicate = [HasVU0];
  string BaseOpcode = opstr;
  let DecoderNamespace = "R5900";
}

class PseudoVU0<dag outs, dag ins, list<dag> pattern,
                InstrItinClass itin = IIPseudo>
    : MipsPseudo<outs, ins, pattern, itin> {
  let ASEPredicate = [HasVU0];
}

//===----------------------------------------------------------------------===//
// R5900 VU0 (COP2) Instruction Formats - Macro Mode
//===----------------------------------------------------------------------===//

// VU0 MacroOP Type 1: 3 VF registers, 6-bit opcode
// Used for: VADD, VSUB, VMUL, VMADD, VMSUB, etc.
// | 31-26  | 25 | 24-21 | 20-16 | 15-11 | 10-6 | 5-0    |
// | COP2   | co | dest  | ft    | fs    | fd   | opcode |
// | 010010 |  1 | 4bits | 5bits | 5bits | 5bits| 6bits  |
class VU0_Type1_FM<bits<6> funct> : StdArch {
  bits<4> dest;  // xyzw destination mask
  bits<5> ft;
  bits<5> fs;
  bits<5> fd;

  bits<32> Inst;

  let Inst{31-26} = 0b010010; // COP2 opcode
  let Inst{25}    = 1;        // co bit
  let Inst{24-21} = dest;
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Inst{10-6}  = fd;
  let Inst{5-0}   = funct;
}

// VU0 MacroOP Type 0: 3 VF registers, 4-bit opcode, 2-bit bc (broadcast)
// Used for: VADDbc, VSUBbc, VMULbc, VMADDbc, VMSUBbc, etc.
// | 31-26  | 25 | 24-21 | 20-16 | 15-11 | 10-6 | 5-2    | 1-0 |
// | COP2   | co | dest  | ft    | fs    | fd   | opcode | bc  |
// | 010010 |  1 | 4bits | 5bits | 5bits | 5bits| 4bits  |2bits|
class VU0_Type0_FM<bits<4> funct, bits<2> bc> : StdArch {
  bits<4> dest;  // xyzw destination mask
  bits<5> ft;
  bits<5> fs;
  bits<5> fd;

  bits<32> Inst;

  let Inst{31-26} = 0b010010; // COP2 opcode
  let Inst{25}    = 1;        // co bit
  let Inst{24-21} = dest;
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Inst{10-6}  = fd;
  let Inst{5-2}   = funct;
  let Inst{1-0}   = bc;
}

// VU0 MacroOP Type 3: 2 VF registers, 11-bit opcode (ACC destination)
// Used for: VADDA, VSUBA, VMULA, VMADDA, VMSUBA, etc.
// | 31-26  | 25 | 24-21 | 20-16 | 15-11 | 10-0   |
// | COP2   | co | dest  | ft    | fs    | opcode |
// | 010010 |  1 | 4bits | 5bits | 5bits | 11bits |
class VU0_Type3_FM<bits<11> funct> : StdArch {
  bits<4> dest;  // xyzw destination mask
  bits<5> ft;
  bits<5> fs;

  bits<32> Inst;

  let Inst{31-26} = 0b010010; // COP2 opcode
  let Inst{25}    = 1;        // co bit
  let Inst{24-21} = dest;
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Inst{10-0}  = funct;
}

// VU0 MacroOP Type 2: 2 VF registers, 9-bit opcode, 2-bit bc (ACC with broadcast)
// Used for: VADDAbc, VSUBAbc, VMULAbc, VMADDAbc, VMSUBAbc, etc.
// | 31-26  | 25 | 24-21 | 20-16 | 15-11 | 10-2   | 1-0 |
// | COP2   | co | dest  | ft    | fs    | opcode | bc  |
// | 010010 |  1 | 4bits | 5bits | 5bits | 9bits  |2bits|
class VU0_Type2_FM<bits<9> funct, bits<2> bc> : StdArch {
  bits<4> dest;  // xyzw destination mask
  bits<5> ft;
  bits<5> fs;

  bits<32> Inst;

  let Inst{31-26} = 0b010010; // COP2 opcode
  let Inst{25}    = 1;        // co bit
  let Inst{24-21} = dest;
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Inst{10-2}  = funct;
  let Inst{1-0}   = bc;
}
