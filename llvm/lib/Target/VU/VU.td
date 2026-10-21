//===-- VU.td - Describe the PS2 Vector Unit target --------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The Vector Units (VU0 and VU1) of the PlayStation 2 Emotion Engine, running
// in micro mode. A micro instruction is a 64-bit word holding an upper (FMAC)
// instruction and a lower (load/store, integer, branch, FDIV, EFU, ...)
// instruction that execute in parallel.
//
//===----------------------------------------------------------------------===//

include "llvm/Target/Target.td"

//===----------------------------------------------------------------------===//
// Subtarget features
//===----------------------------------------------------------------------===//

def FeatureVU1
    : SubtargetFeature<"vu1", "HasVU1", "true",
                       "VU1 only instructions (EFU, XGKICK, XTOP)">;

def HasVU1 : Predicate<"Subtarget->hasVU1()">,
             AssemblerPredicate<(all_of FeatureVU1), "vu1">;

//===----------------------------------------------------------------------===//
// Registers and instructions
//===----------------------------------------------------------------------===//

include "VURegisterInfo.td"
include "VUSchedule.td"
include "VUInstrInfo.td"

defm : RemapAllTargetPseudoPointerOperands<VI>;

def VUInstrInfo : InstrInfo;

//===----------------------------------------------------------------------===//
// Processors
//===----------------------------------------------------------------------===//

def : ProcessorModel<"generic", VUModel, [FeatureVU1]>;
def : ProcessorModel<"vu0", VUModel, []>;
def : ProcessorModel<"vu1", VUModel, [FeatureVU1]>;

//===----------------------------------------------------------------------===//
// Assembly parser and printer
//===----------------------------------------------------------------------===//

def VUAsmParser : AsmParser {
  let ShouldEmitMatchRegisterName = 0;
}

def VUAsmParserVariant : AsmParserVariant {
  // Split "(vi1++)" and "(--vi1)" into separate tokens.
  let TokenizingCharacters = "()+-";
}

def VUInstPrinter : AsmWriter {
  string AsmWriterClassName = "InstPrinter";
}

def VU : Target {
  let InstructionSet = VUInstrInfo;
  let AssemblyWriters = [VUInstPrinter];
  let AssemblyParsers = [VUAsmParser];
  let AssemblyParserVariants = [VUAsmParserVariant];
}
