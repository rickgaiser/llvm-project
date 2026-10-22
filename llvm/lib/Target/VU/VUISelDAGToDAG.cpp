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

private:
  void selectAccSum(SDNode *N);
  void selectDivSqrt(SDNode *N);
  std::pair<SDValue, SDValue> selectField(SDValue V, const SDLoc &DL);

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

  switch (N->getOpcode()) {
  case VUISD::ACCSUM:
    selectAccSum(N);
    return;
  case ISD::FDIV:
  case ISD::FSQRT:
    if (N->getValueType(0) == MVT::f32) {
      selectDivSqrt(N);
      return;
    }
    break;
  }
  SelectCode(N);
}

// MULA, MADDA/MSUBA..., MADD/MSUB. Each instruction reads the ACC value of
// the one before it.
void VUDAGToDAGISel::selectAccSum(SDNode *N) {
  // Opcodes for the x, y, z and w broadcasts and for all fields.
  static constexpr unsigned MulA[] = {VU::MULAx, VU::MULAy, VU::MULAz,
                                      VU::MULAw, VU::MULA};
  static constexpr unsigned MaddA[] = {VU::MADDAx, VU::MADDAy, VU::MADDAz,
                                       VU::MADDAw, VU::MADDA};
  static constexpr unsigned MsubA[] = {VU::MSUBAx, VU::MSUBAy, VU::MSUBAz,
                                       VU::MSUBAw, VU::MSUBA};
  static constexpr unsigned Madd[] = {VU::MADDx, VU::MADDy, VU::MADDz,
                                      VU::MADDw, VU::MADD};
  static constexpr unsigned Msub[] = {VU::MSUBx, VU::MSUBy, VU::MSUBz,
                                      VU::MSUBw, VU::MSUB};

  SDLoc DL(N);
  SDValue Mask = CurDAG->getTargetConstant(15, DL, MVT::i32);
  unsigned NumTerms = N->getNumOperands() / 4;
  SDValue Acc;
  for (unsigned I = 0; I != NumTerms; ++I) {
    SDValue A = N->getOperand(4 * I), B = N->getOperand(4 * I + 1);
    unsigned Field = N->getConstantOperandVal(4 * I + 2);
    bool Neg = N->getConstantOperandVal(4 * I + 3);
    SmallVector<SDValue, 4> Ops;
    if (Acc)
      Ops.push_back(Acc);
    Ops.append({Mask, A, B});
    if (I + 1 == NumTerms) {
      unsigned Opc = Neg ? Msub[Field] : Madd[Field];
      ReplaceNode(N, CurDAG->getMachineNode(Opc, DL, MVT::v4f32, Ops));
      return;
    }
    unsigned Opc = I == 0 ? MulA[Field] : Neg ? MsubA[Field] : MaddA[Field];
    Acc = SDValue(CurDAG->getMachineNode(Opc, DL, MVT::v4f32, Ops), 0);
  }
}

/// Returns the register and field of a scalar operand of DIV or SQRT: the
/// field of an extracted element, or vf0.w for 1.0 and vf0.x for 0.0.
std::pair<SDValue, SDValue> VUDAGToDAGISel::selectField(SDValue V,
                                                        const SDLoc &DL) {
  auto Field = [&](unsigned F) {
    return CurDAG->getTargetConstant(F, DL, MVT::i32);
  };
  if (auto *C = dyn_cast<ConstantFPSDNode>(V)) {
    if (C->isExactlyValue(1.0))
      return {CurDAG->getRegister(VU::VF0, MVT::v4f32), Field(3)};
    if (C->isZero() && !C->isNegative())
      return {CurDAG->getRegister(VU::VF0, MVT::v4f32), Field(0)};
  }
  if (V.getOpcode() == ISD::EXTRACT_VECTOR_ELT &&
      isa<ConstantSDNode>(V.getOperand(1)))
    return {V.getOperand(0), Field(V.getConstantOperandVal(1))};
  return {V, Field(0)};
}

// DIV q, a.f, b.f or SQRT q, b.f, then ADDq.x d, vf0, q. There is no WAITQ:
// VUHazards inserts it where Q is read before the result is ready.
void VUDAGToDAGISel::selectDivSqrt(SDNode *N) {
  SDLoc DL(N);
  SmallVector<SDValue, 4> Ops;
  for (const SDValue &Op : N->ops()) {
    auto [Reg, Field] = selectField(Op, DL);
    Ops.push_back(Reg);
    Ops.push_back(Field);
  }
  unsigned Opc = N->getOpcode() == ISD::FDIV ? VU::DIV : VU::SQRT;
  SDValue Q(CurDAG->getMachineNode(Opc, DL, MVT::f32, Ops), 0);
  SDValue MoveOps[] = {CurDAG->getTargetConstant(8, DL, MVT::i32),
                       CurDAG->getRegister(VU::VF0, MVT::v4f32), Q};
  ReplaceNode(N, CurDAG->getMachineNode(VU::ADDq, DL, MVT::f32, MoveOps));
}
