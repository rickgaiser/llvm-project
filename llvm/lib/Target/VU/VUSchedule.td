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
// packetizer automaton and the hazard recognizer. Operand latencies depend on
// the register that is passed (VF, VI, ACC, I, Q, P) and are computed by
// VUInstrInfo::getOperandLatency.
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
           InstrItinData<IIC_VUUpper2, [InstrStage<1, [VU_UPPER]>]>],
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

def VUWriteUpper : SchedWriteRes<[VUUpperRes]>;
def VUWriteLower : SchedWriteRes<[VULowerRes]>;
def VUWriteUpper2 : SchedWriteRes<[VUUpperRes]> {
  let ReleaseAtCycles = [2];
  let NumMicroOps = 2;
}
def VUWriteLoi : SchedWriteRes<[VULowerRes, VUUpperRes]> {
  let NumMicroOps = 2;
}
def VUWriteFdiv7 : SchedWriteRes<[VULowerRes, VUFdivRes]> {
  let ReleaseAtCycles = [1, 7];
}
def VUWriteFdiv13 : SchedWriteRes<[VULowerRes, VUFdivRes]> {
  let ReleaseAtCycles = [1, 13];
}

def : ItinRW<[VUWriteUpper], [IIC_VUUpper]>;
def : ItinRW<[VUWriteLower], [IIC_VULower]>;
def : ItinRW<[VUWriteUpper2], [IIC_VUUpper2]>;
def : ItinRW<[VUWriteLoi], [IIC_VULoi]>;
def : ItinRW<[VUWriteFdiv7], [IIC_VUFdiv7]>;
def : ItinRW<[VUWriteFdiv13], [IIC_VUFdiv13]>;
foreach N = [10, 11, 17, 23, 28, 43, 53] in {
  def VUWriteEfu # N : SchedWriteRes<[VULowerRes, VUEfuRes]> {
    let ReleaseAtCycles = [1, N];
  }
  def : ItinRW<[!cast<SchedWriteRes>("VUWriteEfu" # N)],
               [!cast<InstrItinClass>("IIC_VUEfu" # N)]>;
}
} // SchedModel = VUModel
