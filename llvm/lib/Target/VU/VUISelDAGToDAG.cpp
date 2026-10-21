//===-- VUISelDAGToDAG.cpp - A DAG to DAG instruction selector for VU -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUMCTargetDesc.h"
#include "VU.h"
#include "VUSelectionDAGInfo.h"
#include "VUSubtarget.h"
#include "VUTargetMachine.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/IR/IntrinsicsVU.h"

using namespace llvm;

#define DEBUG_TYPE "vu-isel"
#define PASS_NAME "VU DAG->DAG Pattern Instruction Selection"

namespace {
class VUDAGToDAGISel : public SelectionDAGISel {
  const VUSubtarget *Subtarget = nullptr;

public:
  VUDAGToDAGISel(VUTargetMachine &TM, CodeGenOptLevel OptLevel)
      : SelectionDAGISel(TM, OptLevel) {}

  bool runOnMachineFunction(MachineFunction &MF) override {
    Subtarget = &MF.getSubtarget<VUSubtarget>();
    return SelectionDAGISel::runOnMachineFunction(MF);
  }

  void Select(SDNode *N) override;

#include "VUGenDAGISel.inc"
};

class VUDAGToDAGISelLegacy : public SelectionDAGISelLegacy {
public:
  static char ID;
  VUDAGToDAGISelLegacy(VUTargetMachine &TM, CodeGenOptLevel OptLevel)
      : SelectionDAGISelLegacy(ID,
                               std::make_unique<VUDAGToDAGISel>(TM, OptLevel)) {
  }
};
} // namespace

char VUDAGToDAGISelLegacy::ID = 0;

INITIALIZE_PASS(VUDAGToDAGISelLegacy, DEBUG_TYPE, PASS_NAME, false, false)

FunctionPass *llvm::createVUISelDag(VUTargetMachine &TM,
                                    CodeGenOptLevel OptLevel) {
  return new VUDAGToDAGISelLegacy(TM, OptLevel);
}

void VUDAGToDAGISel::Select(SDNode *N) {
  if (N->isMachineOpcode()) {
    N->setNodeId(-1);
    return;
  }
  SelectCode(N);
}
