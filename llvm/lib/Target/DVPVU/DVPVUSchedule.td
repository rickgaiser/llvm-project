//===- DVPVUSchedule.td - DVPVU Scheduling Definitions -----*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// PS2 VU Scheduling Model
//
// The VU is a VLIW processor with two pipelines:
// - Upper: FMAC operations (4-cycle latency)
// - Lower: Various operations (FDIV, LSU, IALU, BRU, EFU, RANDU)
//
// Both pipelines execute in parallel from a 64-bit instruction word.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Functional Units
//===----------------------------------------------------------------------===//

// Upper pipeline (FMAC)
def FMAC_FU : FuncUnit;

// Lower pipeline units
def FDIV_FU  : FuncUnit;   // Division/sqrt unit
def LSU_FU   : FuncUnit;   // Load/store unit
def IALU_FU  : FuncUnit;   // Integer ALU
def BRU_FU   : FuncUnit;   // Branch unit
def EFU_FU   : FuncUnit;   // Elementary function unit (VU1 only)
def RANDU_FU : FuncUnit;   // Random number generator

//===----------------------------------------------------------------------===//
// Instruction Itinerary Classes
//===----------------------------------------------------------------------===//

def IIC_FMAC    : InstrItinClass;  // FMAC operations (4 cycles)
def IIC_FDIV    : InstrItinClass;  // DIV (7 cycles)
def IIC_FSQRT   : InstrItinClass;  // SQRT (7 cycles)
def IIC_FRSQRT  : InstrItinClass;  // RSQRT (13 cycles)
def IIC_LSU     : InstrItinClass;  // Load/store (4 cycles)
def IIC_IALU    : InstrItinClass;  // Integer ALU (2 cycles)
def IIC_BRU     : InstrItinClass;  // Branch (2 cycles + delay slot)
def IIC_EFU     : InstrItinClass;  // EFU operations (variable)
def IIC_RANDU   : InstrItinClass;  // Random (1 cycle)
def IIC_NOP     : InstrItinClass;  // NOP

//===----------------------------------------------------------------------===//
// Processor Itineraries
//===----------------------------------------------------------------------===//

def DVPVUItinerary : ProcessorItineraries<
  [FMAC_FU, FDIV_FU, LSU_FU, IALU_FU, BRU_FU, EFU_FU, RANDU_FU],
  [],
  [
    // Upper pipeline - FMAC (4 cycle latency)
    InstrItinData<IIC_FMAC,   [InstrStage<4, [FMAC_FU]>]>,

    // Lower pipeline - FDIV unit
    InstrItinData<IIC_FDIV,   [InstrStage<7, [FDIV_FU]>]>,
    InstrItinData<IIC_FSQRT,  [InstrStage<7, [FDIV_FU]>]>,
    InstrItinData<IIC_FRSQRT, [InstrStage<13, [FDIV_FU]>]>,

    // Lower pipeline - Load/Store (4 cycle latency)
    InstrItinData<IIC_LSU,    [InstrStage<4, [LSU_FU]>]>,

    // Lower pipeline - Integer ALU (2 cycle latency)
    InstrItinData<IIC_IALU,   [InstrStage<2, [IALU_FU]>]>,

    // Lower pipeline - Branch (2 cycles + 1 delay slot)
    InstrItinData<IIC_BRU,    [InstrStage<2, [BRU_FU]>]>,

    // Lower pipeline - EFU (variable, up to 29 cycles for ELENG)
    InstrItinData<IIC_EFU,    [InstrStage<29, [EFU_FU]>]>,

    // Lower pipeline - Random (1 cycle)
    InstrItinData<IIC_RANDU,  [InstrStage<1, [RANDU_FU]>]>,

    // NOP
    InstrItinData<IIC_NOP,    [InstrStage<1, [FMAC_FU, IALU_FU]>]>
  ]>;

//===----------------------------------------------------------------------===//
// Scheduling Model
//===----------------------------------------------------------------------===//

def DVPVUSchedModel : SchedMachineModel {
  // VLIW with 2 slots (Upper + Lower)
  let IssueWidth = 2;

  // Load latency
  let LoadLatency = 4;

  // Branch misprediction penalty (delay slot)
  let MispredictPenalty = 3;

  // Use itineraries
  let Itineraries = DVPVUItinerary;

  // Model is not complete yet
  let CompleteModel = 0;

  // No post-RA scheduling yet
  let PostRAScheduler = 0;

  // No micro-op buffering
  let MicroOpBufferSize = 0;
  let LoopMicroOpBufferSize = 0;
}

//===----------------------------------------------------------------------===//
// Write Resources
//===----------------------------------------------------------------------===//

def FMAC : ProcResource<1> { let BufferSize = 0; }
def FDIV : ProcResource<1> { let BufferSize = 0; }
def LSU  : ProcResource<1> { let BufferSize = 0; }
def IALU : ProcResource<1> { let BufferSize = 0; }
def BRU  : ProcResource<1> { let BufferSize = 0; }
def EFU  : ProcResource<1> { let BufferSize = 0; }

def WriteFMAC   : SchedWrite;
def WriteFDIV   : SchedWrite;
def WriteFSQRT  : SchedWrite;
def WriteFRSQRT : SchedWrite;
def WriteLSU    : SchedWrite;
def WriteIALU   : SchedWrite;
def WriteBRU    : SchedWrite;
def WriteEFU    : SchedWrite;

let SchedModel = DVPVUSchedModel in {
  def : WriteRes<WriteFMAC,   [FMAC]> { let Latency = 4; }
  def : WriteRes<WriteFDIV,   [FDIV]> { let Latency = 7; }
  def : WriteRes<WriteFSQRT,  [FDIV]> { let Latency = 7; }
  def : WriteRes<WriteFRSQRT, [FDIV]> { let Latency = 13; }
  def : WriteRes<WriteLSU,    [LSU]>  { let Latency = 4; }
  def : WriteRes<WriteIALU,   [IALU]> { let Latency = 2; }
  def : WriteRes<WriteBRU,    [BRU]>  { let Latency = 2; }
  def : WriteRes<WriteEFU,    [EFU]>  { let Latency = 29; }
}
