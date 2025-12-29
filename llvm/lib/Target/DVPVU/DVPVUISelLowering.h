//===-- DVPVUISelLowering.h - DVPVU DAG Lowering Interface ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the interfaces that DVPVU uses to lower LLVM code into a
// selection DAG.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_DVPVU_DVPVUISELLOWERING_H
#define LLVM_LIB_TARGET_DVPVU_DVPVUISELLOWERING_H

#include "llvm/CodeGen/TargetLowering.h"

namespace llvm {

class DVPVUSubtarget;

namespace DVPVUISD {
enum NodeType : unsigned {
  FIRST_NUMBER = ISD::BUILTIN_OP_END,
  RET_GLUE,      // Return with glue
  CALL,          // Function call
  WRAPPER,       // Address wrapper
  BROADCAST,     // Broadcast scalar to vector
  DIV_START,     // Start division
  SQRT_START,    // Start square root
  RSQRT_START,   // Start reciprocal square root
  WAITQ,         // Wait for Q register
  WAITP,         // Wait for P register (EFU)
  MR32,          // Rotate right by 32 bits
  MFIR,          // Move from integer register
  MTIR,          // Move to integer register
  ESIN,          // Elementary sine (VU1)
  ECOS,          // Elementary cosine (VU1)
  EEXP,          // Elementary exp (VU1)
  ELOG,          // Elementary log (VU1)
};
} // end namespace DVPVUISD

class DVPVUTargetLowering : public TargetLowering {
  const DVPVUSubtarget &Subtarget;

public:
  explicit DVPVUTargetLowering(const TargetMachine &TM,
                                const DVPVUSubtarget &STI);

  const char *getTargetNodeName(unsigned Opcode) const override;

  SDValue LowerOperation(SDValue Op, SelectionDAG &DAG) const override;

  SDValue LowerFormalArguments(SDValue Chain, CallingConv::ID CallConv,
                                bool IsVarArg,
                                const SmallVectorImpl<ISD::InputArg> &Ins,
                                const SDLoc &DL, SelectionDAG &DAG,
                                SmallVectorImpl<SDValue> &InVals) const override;

  SDValue LowerReturn(SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
                      const SmallVectorImpl<ISD::OutputArg> &Outs,
                      const SmallVectorImpl<SDValue> &OutVals, const SDLoc &DL,
                      SelectionDAG &DAG) const override;

  SDValue LowerCall(TargetLowering::CallLoweringInfo &CLI,
                    SmallVectorImpl<SDValue> &InVals) const override;

  bool CanLowerReturn(CallingConv::ID CallConv, MachineFunction &MF,
                      bool IsVarArg,
                      const SmallVectorImpl<ISD::OutputArg> &Outs,
                      LLVMContext &Context, const Type *RetTy) const override;

  EVT getSetCCResultType(const DataLayout &DL, LLVMContext &Context,
                         EVT VT) const override;

  bool isOffsetFoldingLegal(const GlobalAddressSDNode *GA) const override {
    return false;
  }

  Register getRegisterByName(const char *RegName, LLT VT,
                              const MachineFunction &MF) const override;

private:
  SDValue LowerGlobalAddress(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerConstantPool(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerFDIV(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerFSQRT(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerVECTOR_SHUFFLE(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerBUILD_VECTOR(SDValue Op, SelectionDAG &DAG) const;
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_DVPVU_DVPVUISELLOWERING_H
