//===-- VUInstrFormats.td - VU instruction formats ---------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Each instruction below is one half of a 64-bit micro instruction word: the
// lower instruction occupies bits 31-0 and the upper instruction bits 63-32.
// The two halves are combined into a BUNDLE by the assembler, the code emitter
// and the disassembler. Bits 31-27 of the upper word hold the I, E, M, D and T
// flags, which belong to the bundle rather than to the upper instruction.
//
//===----------------------------------------------------------------------===//

class VUInst<dag outs, dag ins, string asmstr> : Instruction {
  field bits<32> Inst;
  field bits<32> SoftFail = 0;

  let Namespace = "VU";
  let Size = 4;
  let OutOperandList = outs;
  let InOperandList = ins;
  let AsmString = asmstr;
  let hasSideEffects = 0;

  // Which half of the micro instruction word this instruction occupies.
  bit IsLower = 0;
  let TSFlags{0} = IsLower;
}

//===----------------------------------------------------------------------===//
// Upper instructions
//===----------------------------------------------------------------------===//

class VUUpper<dag outs, dag ins, string asmstr>
    : VUInst<outs, ins, asmstr> {
  let DecoderNamespace = "Upper";
  let Itinerary = IIC_VUUpper;
  let Inst{31-25} = 0;
}

// Upper instruction with a 6-bit opcode in bits 5-0.
class VUUpperOp<bits<6> op, dag outs, dag ins, string asmstr>
    : VUUpper<outs, ins, asmstr> {
  let Inst{5-0} = op;
}

// Upper instruction from the extended opcode table: bits 5-2 are all ones and
// the 7-bit index is split over bits 10-6 and bits 1-0.
class VUUpperExt<bits<7> idx, dag outs, dag ins, string asmstr>
    : VUUpper<outs, ins, asmstr> {
  let Inst{10-6} = idx{6-2};
  let Inst{5-2} = 0b1111;
  let Inst{1-0} = idx{1-0};
}

//===----------------------------------------------------------------------===//
// Lower instructions
//===----------------------------------------------------------------------===//

class VULower<dag outs, dag ins, string asmstr> : VUInst<outs, ins, asmstr> {
  let IsLower = 1;
  let DecoderNamespace = "Lower";
  let Itinerary = IIC_VULower;
}

// Lower instruction with a 7-bit opcode in bits 31-25.
class VULowerOp<bits<7> op, dag outs, dag ins, string asmstr>
    : VULower<outs, ins, asmstr> {
  let Inst{31-25} = op;
}

// Lower instruction with opcode 0x40 and a 6-bit function in bits 5-0.
class VULowerFn<bits<6> fn, dag outs, dag ins, string asmstr>
    : VULowerOp<0b1000000, outs, ins, asmstr> {
  let Inst{5-0} = fn;
}

// Lower instruction with opcode 0x40 from the extended table: bits 5-2 are
// all ones and the 7-bit index is split over bits 10-6 and bits 1-0.
class VULowerExt<bits<7> idx, dag outs, dag ins, string asmstr>
    : VULowerOp<0b1000000, outs, ins, asmstr> {
  let Inst{10-6} = idx{6-2};
  let Inst{5-2} = 0b1111;
  let Inst{1-0} = idx{1-0};
}
