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
//===----------------------------------------------------------------------===//

def VU_UPPER : FuncUnit;
def VU_LOWER : FuncUnit;

def IIC_VUUpper : InstrItinClass;
def IIC_VULower : InstrItinClass;

def VUItineraries
    : ProcessorItineraries<[VU_UPPER, VU_LOWER], [], [
        InstrItinData<IIC_VUUpper, [InstrStage<1, [VU_UPPER]>]>,
        InstrItinData<IIC_VULower, [InstrStage<1, [VU_LOWER]>]>
      ]>;

def VUModel : SchedMachineModel {
  let IssueWidth = 2;
  let MicroOpBufferSize = 0; // In-order.
  let LoadLatency = 4;
  let CompleteModel = 0;
  let Itineraries = VUItineraries;
}
