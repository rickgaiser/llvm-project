//===-- VUISelLowering.h - VU DAG lowering interface ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_VU_VUISELLOWERING_H
#define LLVM_LIB_TARGET_VU_VUISELLOWERING_H

#include "llvm/CodeGen/TargetLowering.h"

namespace llvm {

class VUSubtarget;

class VUTargetLowering : public TargetLowering {
  const VUSubtarget &Subtarget;

public:
  VUTargetLowering(const TargetMachine &TM, const VUSubtarget &STI);

  EVT getSetCCResultType(const DataLayout &DL, LLVMContext &Context,
                         EVT VT) const override;

  SDValue LowerOperation(SDValue Op, SelectionDAG &DAG) const override;

  /// Any f32 constant can be loaded with LOI.
  bool isFPImmLegal(const APFloat &Imm, EVT VT,
                    bool ForCodeSize) const override {
    return VT == MVT::f32;
  }

  SDValue LowerFormalArguments(SDValue Chain, CallingConv::ID CallConv,
                               bool IsVarArg,
                               const SmallVectorImpl<ISD::InputArg> &Ins,
                               const SDLoc &DL, SelectionDAG &DAG,
                               SmallVectorImpl<SDValue> &InVals) const override;

  SDValue LowerReturn(SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
                      const SmallVectorImpl<ISD::OutputArg> &Outs,
                      const SmallVectorImpl<SDValue> &OutVals, const SDLoc &DL,
                      SelectionDAG &DAG) const override;

  SDValue LowerCall(CallLoweringInfo &CLI,
                    SmallVectorImpl<SDValue> &InVals) const override;

private:
  SDValue lowerBUILD_VECTOR(SDValue Op, SelectionDAG &DAG) const;
  SDValue lowerVectorField(SDValue Op, SelectionDAG &DAG) const;
  SDValue lowerIntrinsic(SDValue Op, SelectionDAG &DAG) const;
  SDValue lowerUnsupported(SDValue Op, SelectionDAG &DAG,
                           const char *What) const;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_VU_VUISELLOWERING_H
