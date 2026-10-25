//===-- VUSchedule.td - VU scheduling definitions ----------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A micro instruction word issues one upper and one lower instruction per
// cycle. The itineraries only describe these two issue slots; they drive the
// packetizer automaton and the hazard recognizer.
//
// The latencies of the results, in words, are those of the writes below, one
// per result (VUInstrInfo::getOperandLatency reads them, and llvm-mca too).
// All are measured on the PS2 (bench/vuuarch, maxsdk docs/vu0.md):
//   FMAC result (ADD, MUL, MADD, bc, i and q forms, MAX, MINI, ABS, FTOI,
//   ITOF, CLIP)                                                         4
//   ACC result (MULA, MADDA, ..., OPMULA) to the next FMAC              1
//   MOVE, MR32, MFIR, MFP, LQ, ILW to a use                             4
//   VI, I and R results (IADD, MTIR, LOI, flag checks, ...)             1
//   DIV, SQRT 7; RSQRT 13; the EFU ops their measured latency (ESADD 10,
//   ESQRT/ESUM/ERCPR 11, ERSADD/ELENG/ERSQRT 17, ERLENG 23, ESIN 28,
//   EEXP 43, EATAN 53), equal to their throughput.
// Q, the MAC and clip flags, and a VI read by a branch in the next word are
// not interlocked on the PS2; the compiler keeps these distances for
// correctness. Results may be written in any order (RetireOOO, for llvm-mca).
//
// The processor resources below also describe the FDIV and EFU units, which
// stay busy for several cycles: the next DIV, SQRT or RSQRT stalls until the
// previous one is done, and likewise for the EFU. The automaton can not
// express this, so the pipeliner uses these resources instead.
//
//===----------------------------------------------------------------------===//

def VU_UPPER : FuncUnit;
def VU_LOWER : FuncUnit;

def IIC_VUUpper : InstrItinClass;
def IIC_VULower : InstrItinClass;
// Pseudos: two upper instructions, or a LOI and an upper instruction.
def IIC_VUUpper2 : InstrItinClass;
def IIC_VULoi : InstrItinClass;
// CLIP and a test of its flags (CLIP_FCAND).
def IIC_VUClipTest : InstrItinClass;
// FDIV: DIV and SQRT, RSQRT.
def IIC_VUFdiv7 : InstrItinClass;
def IIC_VUFdiv13 : InstrItinClass;
// EFU, by throughput.
foreach N = [10, 11, 17, 23, 28, 43, 53] in
  def IIC_VUEfu # N : InstrItinClass;

defvar LowerClasses = [IIC_VULower, IIC_VULoi, IIC_VUFdiv7, IIC_VUFdiv13,
                       IIC_VUEfu10, IIC_VUEfu11, IIC_VUEfu17, IIC_VUEfu23,
                       IIC_VUEfu28, IIC_VUEfu43, IIC_VUEfu53];

def VUItineraries
    : ProcessorItineraries<[VU_UPPER, VU_LOWER], [],
        !listconcat(
          [InstrItinData<IIC_VUUpper, [InstrStage<1, [VU_UPPER]>]>,
           InstrItinData<IIC_VUUpper2, [InstrStage<1, [VU_UPPER]>]>,
           InstrItinData<IIC_VUClipTest, [InstrStage<1, [VU_UPPER]>]>],
          !foreach(C, LowerClasses,
                   InstrItinData<C, [InstrStage<1, [VU_LOWER]>]>))>;

def VUModel : SchedMachineModel {
  let IssueWidth = 2;
  let MicroOpBufferSize = 0; // In-order.
  let LoadLatency = 4;
  let CompleteModel = 0;
  let Itineraries = VUItineraries;
}

let SchedModel = VUModel in {
def VUUpperRes : ProcResource<1>;
def VULowerRes : ProcResource<1>;
def VUFdivRes : ProcResource<1>;
def VUEfuRes : ProcResource<1>;

// FMAC result.
def VUWriteUpper : SchedWriteRes<[VUUpperRes]> {
  let Latency = 4;
  let RetireOOO = 1;
}
// ACC result, forwarded to the next FMAC instruction.
def VUWriteAcc : SchedWriteRes<[VUUpperRes]> {
  let Latency = 1;
  let RetireOOO = 1;
}
// VI, I or R result, or none.
def VUWriteLower : SchedWriteRes<[VULowerRes]> {
  let Latency = 1;
  let RetireOOO = 1;
}
// VF result of a lower instruction, or ILW.
def VUWriteLower4 : SchedWriteRes<[VULowerRes]> {
  let Latency = 4;
  let RetireOOO = 1;
}
// WAITQ and WAITP: Q and P may be read in the same word.
def VUWriteWait : SchedWriteRes<[VULowerRes]> {
  let Latency = 0;
  let RetireOOO = 1;
}
// Pseudos: two FMAC instructions (SPLAT), or LOI and an FMAC instruction.
def VUWriteUpper2 : SchedWriteRes<[VUUpperRes]> {
  let Latency = 5;
  let RetireOOO = 1;
  let ReleaseAtCycles = [2];
  let NumMicroOps = 2;
}
def VUWriteLoi : SchedWriteRes<[VULowerRes, VUUpperRes]> {
  let Latency = 5;
  let RetireOOO = 1;
  let NumMicroOps = 2;
}
// Pseudo: CLIP, and the test 4 cycles later (the pipeliner reserves both
// slots in the first cycle).
def VUWriteClipTest : SchedWriteRes<[VUUpperRes, VULowerRes]> {
  let Latency = 5;
  let RetireOOO = 1;
  let NumMicroOps = 2;
}
def VUWriteFdiv7 : SchedWriteRes<[VULowerRes, VUFdivRes]> {
  let Latency = 7;
  let RetireOOO = 1;
  let ReleaseAtCycles = [1, 7];
}
def VUWriteFdiv13 : SchedWriteRes<[VULowerRes, VUFdivRes]> {
  let Latency = 13;
  let RetireOOO = 1;
  let ReleaseAtCycles = [1, 13];
}

def : ItinRW<[VUWriteUpper], [IIC_VUUpper]>;
def : ItinRW<[VUWriteLower], [IIC_VULower]>;
def : ItinRW<[VUWriteUpper2], [IIC_VUUpper2]>;
def : ItinRW<[VUWriteLoi], [IIC_VULoi]>;
def : ItinRW<[VUWriteClipTest], [IIC_VUClipTest]>;
def : ItinRW<[VUWriteFdiv7], [IIC_VUFdiv7]>;
def : ItinRW<[VUWriteFdiv13], [IIC_VUFdiv13]>;
foreach N = [10, 11, 17, 23, 28, 43, 53] in {
  def VUWriteEfu # N : SchedWriteRes<[VULowerRes, VUEfuRes]> {
    let Latency = N;
    let RetireOOO = 1;
    let ReleaseAtCycles = [1, N];
  }
  def : ItinRW<[!cast<SchedWriteRes>("VUWriteEfu" # N)],
               [!cast<InstrItinClass>("IIC_VUEfu" # N)]>;
}

def : InstRW<[VUWriteAcc], (instregex "^(ADD|SUB|MUL|MADD|MSUB)A[iqwxyz]?$",
                                      "^OPMULA$")>;
def : InstRW<[VUWriteLower4],
             (instregex "^(LQ|ILW|ILWR|MOVE|MR32|MFIR|MFP|RGET|INSERT_VI)$")>;
// The loaded VF, and the base register or R.
def : InstRW<[VUWriteLower4, VUWriteLower], (instregex "^(LQI|LQD|RNEXT)$")>;
def : InstRW<[VUWriteWait], (instregex "^WAIT[QP]$")>;
} // SchedModel = VUModel
