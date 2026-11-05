//=- MipsScheduleR3000A.td - R3000A Scheduling Definitions ----*- tablegen -*-=//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Scheduling model for the R3000A (PlayStation 2 IOP core): in-order, single
// issue, with a one-instruction load delay slot. The model exists so the
// scheduler separates a load from the instruction that uses its result;
// otherwise the load-delay pass has to insert a nop. Only what differs from one
// cycle is listed. All numbers are measured on the PS2 IOP: mflo can issue 14
// cycles after mult (7 or 10 for a small rs, which the model cannot know) and 37
// after div, and stores go out at one per 2 cycles, without holding up loads.
//
//===----------------------------------------------------------------------===//

def MipsR3000AModel : SchedMachineModel {
  int IssueWidth = 1;
  int MicroOpBufferSize = 0;

  int LoadLatency = 2;
  int MispredictPenalty = 1;

  list<Predicate> UnsupportedFeatures = [];

  let CompleteModel = 0;
  let PostRAScheduler = 1;
}

let SchedModel = MipsR3000AModel in {

def R3000AALU : ProcResource<1>;
def R3000ALS : ProcResource<1>;
def R3000AMD : ProcResource<1>;
def R3000AWB : ProcResource<1>;

def R3000AWriteALU : SchedWriteRes<[R3000AALU]>;
def R3000AWriteLoad : SchedWriteRes<[R3000ALS]> { let Latency = 2; }
def R3000AWriteStore : SchedWriteRes<[R3000AWB]> {
  let ReleaseAtCycles = [2];
}
def R3000AWriteMul : SchedWriteRes<[R3000AMD]> {
  let Latency = 14;
  let ReleaseAtCycles = [14];
}
def R3000AWriteDiv : SchedWriteRes<[R3000AMD]> {
  let Latency = 37;
  let ReleaseAtCycles = [37];
}

def : InstRW<[R3000AWriteLoad], (instregex "^(LB|LBu|LH|LHu|LW|LWL|LWR|MFC0)$")>;
def : InstRW<[R3000AWriteStore], (instregex "^(SB|SH|SW|SWL|SWR)$")>;
def : InstRW<[R3000AWriteMul], (instregex "^(MULT|MULTu|PseudoMULT|PseudoMULTu)$")>;
def : InstRW<[R3000AWriteDiv], (instregex "^(DIV|DIVu|PseudoSDIV|PseudoUDIV)$")>;

}
