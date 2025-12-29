//===-- DVPVUISelLowering.cpp - DVPVU DAG Lowering Implementation ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the DVPVUTargetLowering class.
//
//===----------------------------------------------------------------------===//

#include "DVPVUISelLowering.h"
#include "DVPVU.h"
#include "DVPVUMachineFunctionInfo.h"
#include "DVPVUSubtarget.h"
#include "DVPVUTargetMachine.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define DEBUG_TYPE "dvpvu-lower"

#define GET_REGINFO_ENUM
#include "DVPVUGenRegisterInfo.inc"

#include "DVPVUGenCallingConv.inc"

DVPVUTargetLowering::DVPVUTargetLowering(const TargetMachine &TM,
                                           const DVPVUSubtarget &STI)
    : TargetLowering(TM, STI), Subtarget(STI) {

  // Set up register classes
  addRegisterClass(MVT::v4f32, &DVPVU::VFRegsRegClass);
  addRegisterClass(MVT::i16, &DVPVU::VIRegsRegClass);

  // Compute derived properties from the register classes
  computeRegisterProperties(STI.getRegisterInfo());

  // Set the default scheduling preference
  setSchedulingPreference(Sched::Source);

  // VU operations
  setOperationAction(ISD::GlobalAddress, MVT::i32, Custom);
  setOperationAction(ISD::ConstantPool, MVT::i32, Custom);

  // Division is a multi-cycle operation via DIV + WAITQ + Q
  setOperationAction(ISD::FDIV, MVT::v4f32, Custom);
  setOperationAction(ISD::FDIV, MVT::f32, Custom);

  // Square root via SQRT/RSQRT + WAITQ + Q
  setOperationAction(ISD::FSQRT, MVT::v4f32, Custom);
  setOperationAction(ISD::FSQRT, MVT::f32, Custom);

  // Vector operations
  setOperationAction(ISD::VECTOR_SHUFFLE, MVT::v4f32, Custom);
  setOperationAction(ISD::BUILD_VECTOR, MVT::v4f32, Custom);

  // No support for certain operations
  setOperationAction(ISD::BR_CC, MVT::i16, Expand);
  setOperationAction(ISD::SELECT_CC, MVT::i16, Expand);
  setOperationAction(ISD::SELECT_CC, MVT::f32, Expand);
  setOperationAction(ISD::SELECT_CC, MVT::v4f32, Expand);

  // Integer operations
  setOperationAction(ISD::MUL, MVT::i16, Expand);
  setOperationAction(ISD::SDIV, MVT::i16, Expand);
  setOperationAction(ISD::UDIV, MVT::i16, Expand);
  setOperationAction(ISD::SREM, MVT::i16, Expand);
  setOperationAction(ISD::UREM, MVT::i16, Expand);

  // Boolean result type
  setBooleanContents(ZeroOrOneBooleanContent);
  setBooleanVectorContents(ZeroOrNegativeOneBooleanContent);

  // Set minimum function alignment
  setMinFunctionAlignment(Align(8));
  setPrefFunctionAlignment(Align(8));
}

const char *DVPVUTargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch ((DVPVUISD::NodeType)Opcode) {
  case DVPVUISD::FIRST_NUMBER:
    break;
  case DVPVUISD::RET_GLUE:
    return "DVPVUISD::RET_GLUE";
  case DVPVUISD::CALL:
    return "DVPVUISD::CALL";
  case DVPVUISD::WRAPPER:
    return "DVPVUISD::WRAPPER";
  case DVPVUISD::BROADCAST:
    return "DVPVUISD::BROADCAST";
  case DVPVUISD::DIV_START:
    return "DVPVUISD::DIV_START";
  case DVPVUISD::SQRT_START:
    return "DVPVUISD::SQRT_START";
  case DVPVUISD::RSQRT_START:
    return "DVPVUISD::RSQRT_START";
  case DVPVUISD::WAITQ:
    return "DVPVUISD::WAITQ";
  case DVPVUISD::WAITP:
    return "DVPVUISD::WAITP";
  case DVPVUISD::MR32:
    return "DVPVUISD::MR32";
  case DVPVUISD::MFIR:
    return "DVPVUISD::MFIR";
  case DVPVUISD::MTIR:
    return "DVPVUISD::MTIR";
  case DVPVUISD::ESIN:
    return "DVPVUISD::ESIN";
  case DVPVUISD::ECOS:
    return "DVPVUISD::ECOS";
  case DVPVUISD::EEXP:
    return "DVPVUISD::EEXP";
  case DVPVUISD::ELOG:
    return "DVPVUISD::ELOG";
  }
  return nullptr;
}

SDValue DVPVUTargetLowering::LowerOperation(SDValue Op,
                                             SelectionDAG &DAG) const {
  switch (Op.getOpcode()) {
  case ISD::GlobalAddress:
    return LowerGlobalAddress(Op, DAG);
  case ISD::ConstantPool:
    return LowerConstantPool(Op, DAG);
  case ISD::FDIV:
    return LowerFDIV(Op, DAG);
  case ISD::FSQRT:
    return LowerFSQRT(Op, DAG);
  case ISD::VECTOR_SHUFFLE:
    return LowerVECTOR_SHUFFLE(Op, DAG);
  case ISD::BUILD_VECTOR:
    return LowerBUILD_VECTOR(Op, DAG);
  default:
    llvm_unreachable("Unexpected operation to lower");
  }
}

SDValue DVPVUTargetLowering::LowerGlobalAddress(SDValue Op,
                                                 SelectionDAG &DAG) const {
  SDLoc DL(Op);
  const GlobalValue *GV = cast<GlobalAddressSDNode>(Op)->getGlobal();
  SDValue GA = DAG.getTargetGlobalAddress(GV, DL, MVT::i32);
  return DAG.getNode(DVPVUISD::WRAPPER, DL, MVT::i32, GA);
}

SDValue DVPVUTargetLowering::LowerConstantPool(SDValue Op,
                                                SelectionDAG &DAG) const {
  SDLoc DL(Op);
  ConstantPoolSDNode *CP = cast<ConstantPoolSDNode>(Op);
  SDValue CPI = DAG.getTargetConstantPool(CP->getConstVal(), MVT::i32,
                                           CP->getAlign(), CP->getOffset());
  return DAG.getNode(DVPVUISD::WRAPPER, DL, MVT::i32, CPI);
}

SDValue DVPVUTargetLowering::LowerFDIV(SDValue Op, SelectionDAG &DAG) const {
  // VU division: DIV Q, fs, ft -> stores result in Q register
  // Need to emit DIV, then WAITQ, then read Q
  SDLoc DL(Op);
  SDValue LHS = Op.getOperand(0);
  SDValue RHS = Op.getOperand(1);
  EVT VT = Op.getValueType();

  // For now, just return the operation - proper lowering will be done later
  // TODO: Implement proper DIV -> WAITQ -> Q sequence
  return SDValue();
}

SDValue DVPVUTargetLowering::LowerFSQRT(SDValue Op, SelectionDAG &DAG) const {
  // Similar to division - SQRT Q, ... -> WAITQ -> read Q
  SDLoc DL(Op);
  EVT VT = Op.getValueType();

  // TODO: Implement proper SQRT -> WAITQ -> Q sequence
  return SDValue();
}

SDValue DVPVUTargetLowering::LowerVECTOR_SHUFFLE(SDValue Op,
                                                  SelectionDAG &DAG) const {
  // VU has limited shuffle support via broadcast and MR32
  SDLoc DL(Op);

  // TODO: Implement vector shuffle lowering
  return SDValue();
}

SDValue DVPVUTargetLowering::LowerBUILD_VECTOR(SDValue Op,
                                                SelectionDAG &DAG) const {
  SDLoc DL(Op);
  EVT VT = Op.getValueType();

  // Check for broadcast (all elements same)
  SDValue SplatValue = Op.getOperand(0);
  bool IsBroadcast = true;
  for (unsigned i = 1; i < Op.getNumOperands(); ++i) {
    if (Op.getOperand(i) != SplatValue) {
      IsBroadcast = false;
      break;
    }
  }

  if (IsBroadcast) {
    // TODO: Implement broadcast instruction pattern
    // For now, let LLVM expand this
    return SDValue();
  }

  // TODO: Handle other BUILD_VECTOR patterns
  return SDValue();
}

SDValue DVPVUTargetLowering::LowerFormalArguments(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &DL,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  MachineFunction &MF = DAG.getMachineFunction();
  MachineRegisterInfo &RegInfo = MF.getRegInfo();

  // Assign locations to all incoming arguments
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, ArgLocs, *DAG.getContext());
  CCInfo.AnalyzeFormalArguments(Ins, CC_DVPVU);

  for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
    CCValAssign &VA = ArgLocs[i];

    if (VA.isRegLoc()) {
      // Argument passed in register
      EVT RegVT = VA.getLocVT();
      const TargetRegisterClass *RC;

      if (RegVT == MVT::v4f32)
        RC = &DVPVU::VFRegsRegClass;
      else if (RegVT == MVT::i16)
        RC = &DVPVU::VIRegsRegClass;
      else
        llvm_unreachable("Unexpected register type");

      Register VReg = RegInfo.createVirtualRegister(RC);
      RegInfo.addLiveIn(VA.getLocReg(), VReg);
      SDValue ArgValue = DAG.getCopyFromReg(Chain, DL, VReg, RegVT);
      InVals.push_back(ArgValue);
    } else {
      // VU doesn't support stack arguments
      llvm_unreachable("VU does not support stack arguments");
    }
  }

  return Chain;
}

SDValue DVPVUTargetLowering::LowerReturn(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs,
    const SmallVectorImpl<SDValue> &OutVals, const SDLoc &DL,
    SelectionDAG &DAG) const {
  SmallVector<CCValAssign, 16> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, DAG.getMachineFunction(), RVLocs,
                 *DAG.getContext());
  CCInfo.AnalyzeReturn(Outs, RetCC_DVPVU);

  SDValue Glue;
  SmallVector<SDValue, 4> RetOps;
  RetOps.push_back(Chain);

  // Copy return values to output registers
  for (unsigned i = 0, e = RVLocs.size(); i != e; ++i) {
    CCValAssign &VA = RVLocs[i];
    assert(VA.isRegLoc() && "Can only return in registers");

    Chain = DAG.getCopyToReg(Chain, DL, VA.getLocReg(), OutVals[i], Glue);
    Glue = Chain.getValue(1);
    RetOps.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
  }

  RetOps[0] = Chain;
  if (Glue.getNode())
    RetOps.push_back(Glue);

  return DAG.getNode(DVPVUISD::RET_GLUE, DL, MVT::Other, RetOps);
}

SDValue DVPVUTargetLowering::LowerCall(TargetLowering::CallLoweringInfo &CLI,
                                        SmallVectorImpl<SDValue> &InVals) const {
  // VU doesn't have traditional function calls
  // Microcode is typically executed in a linear fashion
  llvm_unreachable("VU microcode does not support function calls");
}

bool DVPVUTargetLowering::CanLowerReturn(
    CallingConv::ID CallConv, MachineFunction &MF, bool IsVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs, LLVMContext &Context,
    const Type *RetTy) const {
  SmallVector<CCValAssign, 16> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, RVLocs, Context);
  return CCInfo.CheckReturn(Outs, RetCC_DVPVU);
}

EVT DVPVUTargetLowering::getSetCCResultType(const DataLayout &DL,
                                             LLVMContext &Context,
                                             EVT VT) const {
  if (VT.isVector())
    return VT.changeVectorElementTypeToInteger();
  return MVT::i16;
}

Register DVPVUTargetLowering::getRegisterByName(const char *RegName, LLT VT,
                                                 const MachineFunction &MF) const {
  // Parse register name
  StringRef Name(RegName);

  if (Name.starts_with_insensitive("vf")) {
    unsigned RegNum;
    if (!Name.substr(2).getAsInteger(10, RegNum) && RegNum < 32)
      return DVPVU::VF0 + RegNum;
  }

  if (Name.starts_with_insensitive("vi")) {
    unsigned RegNum;
    if (!Name.substr(2).getAsInteger(10, RegNum) && RegNum < 16)
      return DVPVU::VI0 + RegNum;
  }

  if (Name.equals_insensitive("acc"))
    return DVPVU::ACC;
  if (Name.equals_insensitive("i"))
    return DVPVU::I_REG;
  if (Name.equals_insensitive("q"))
    return DVPVU::Q;
  if (Name.equals_insensitive("p"))
    return DVPVU::P;
  if (Name.equals_insensitive("r"))
    return DVPVU::R;

  return Register();
}
