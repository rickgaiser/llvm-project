//===- DVPVU.td - Describe the DVPVU Target Machine --------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file describes the PS2 Vector Unit (VU) target machine.
//
// The VU is a VLIW processor with 64-bit instruction words containing
// an Upper instruction (FMAC) and Lower instruction (various) that
// execute in parallel.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Target-independent interfaces which we are implementing
//===----------------------------------------------------------------------===//

include "llvm/Target/Target.td"

//===----------------------------------------------------------------------===//
// DVPVU Subtarget Features
//===----------------------------------------------------------------------===//

def FeatureVU1 : SubtargetFeature<"vu1", "IsVU1", "true",
    "VU1 mode (16KB memory, EFU support)">;

def FeatureEFU : SubtargetFeature<"efu", "HasEFU", "true",
    "Has Elementary Function Unit (VU1 only)",
    [FeatureVU1]>;

//===----------------------------------------------------------------------===//
// Register File, Calling Conv, Instruction Descriptions
//===----------------------------------------------------------------------===//

include "DVPVUSchedule.td"
include "DVPVURegisterInfo.td"
include "DVPVUCallingConv.td"
include "DVPVUInstrInfo.td"

def DVPVUInstrInfo : InstrInfo;

//===----------------------------------------------------------------------===//
// DVPVU processors supported.
//===----------------------------------------------------------------------===//

def : ProcessorModel<"vu0", DVPVUSchedModel, []>;
def : ProcessorModel<"vu1", DVPVUSchedModel, [FeatureVU1, FeatureEFU]>;

def DVPVUInstPrinter : AsmWriter {
  string AsmWriterClassName = "InstPrinter";
  bit isMCAsmWriter = 1;
}

//===----------------------------------------------------------------------===//
// Declare the target which we are implementing
//===----------------------------------------------------------------------===//

def DVPVU : Target {
  let InstructionSet = DVPVUInstrInfo;
  let AssemblyWriters = [DVPVUInstPrinter];
}
