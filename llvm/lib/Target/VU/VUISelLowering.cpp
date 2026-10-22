//===-- VUISelLowering.cpp - VU DAG lowering implementation ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A micro program is a function without arguments or return value. Memory is
// accessed through the llvm.vu.* intrinsics, which take quadword addresses.
//
//===----------------------------------------------------------------------===//

#include "VUISelLowering.h"
#include "MCTargetDesc/VUMCTargetDesc.h"
#include "VUSelectionDAGInfo.h"
#include "VUSubtarget.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IntrinsicsVU.h"

using namespace llvm;

#define DEBUG_TYPE "vu-lower"

VUTargetLowering::VUTargetLowering(const TargetMachine &TM,
                                   const VUSubtarget &STI)
    : TargetLowering(TM, STI), Subtarget(STI) {
  addRegisterClass(MVT::v4f32, &VU::VFRegClass);
  addRegisterClass(MVT::v4i32, &VU::VFRegClass);
  addRegisterClass(MVT::f32, &VU::VFRegClass);
  addRegisterClass(MVT::i16, &VU::VIRegClass);
  computeRegisterProperties(STI.getRegisterInfo());

  setBooleanContents(ZeroOrOneBooleanContent);
  setMinFunctionAlignment(Align(8));
  setPrefFunctionAlignment(Align(8));

  // Only the operations below are supported on vectors.
  for (MVT VT : {MVT::v4f32, MVT::v4i32}) {
    for (unsigned Op = 0; Op < ISD::BUILTIN_OP_END; ++Op)
      setOperationAction(Op, VT, Expand);
    for (unsigned Op : {ISD::BITCAST, ISD::SINT_TO_FP, ISD::FP_TO_SINT})
      setOperationAction(Op, VT, Legal);
    for (unsigned Op : {ISD::LOAD, ISD::STORE})
      setOperationAction(Op, VT, Custom);
    for (MVT Other : MVT::all_valuetypes()) {
      setTruncStoreAction(VT, Other, Expand);
      for (auto Ext : {ISD::EXTLOAD, ISD::SEXTLOAD, ISD::ZEXTLOAD})
        setLoadExtAction(Ext, VT, Other, Expand);
    }
  }
  for (unsigned Op : {ISD::FADD, ISD::FSUB, ISD::FMUL, ISD::FMA, ISD::FABS,
                      ISD::FNEG, ISD::FMAXNUM, ISD::FMINNUM})
    setOperationAction(Op, MVT::v4f32, Legal);
  for (unsigned Op : {ISD::BUILD_VECTOR, ISD::SCALAR_TO_VECTOR,
                      ISD::INSERT_VECTOR_ELT, ISD::EXTRACT_VECTOR_ELT})
    setOperationAction(Op, MVT::v4f32, Custom);

  // Scalar floats.
  for (unsigned Op :
       {ISD::FREM,      ISD::FPOW,     ISD::FSIN,      ISD::FCOS,
        ISD::FSINCOS,   ISD::FEXP,     ISD::FEXP2,     ISD::FLOG,
        ISD::FLOG2,     ISD::FLOG10,   ISD::FCOPYSIGN, ISD::FFLOOR,
        ISD::FCEIL,     ISD::FTRUNC,   ISD::FRINT,     ISD::FNEARBYINT,
        ISD::FROUND,    ISD::FMINIMUM, ISD::FMAXIMUM,  ISD::SELECT,
        ISD::SELECT_CC, ISD::SETCC,    ISD::BR_CC})
    setOperationAction(Op, MVT::f32, Expand);
  for (unsigned Op : {ISD::LOAD, ISD::STORE})
    setOperationAction(Op, MVT::f32, Custom);

  // 16-bit integers.
  for (unsigned Op : {ISD::MUL,
                      ISD::MULHS,
                      ISD::MULHU,
                      ISD::SDIV,
                      ISD::UDIV,
                      ISD::SREM,
                      ISD::UREM,
                      ISD::SDIVREM,
                      ISD::UDIVREM,
                      ISD::SMUL_LOHI,
                      ISD::UMUL_LOHI,
                      ISD::SRA,
                      ISD::SRL,
                      ISD::ROTL,
                      ISD::ROTR,
                      ISD::CTPOP,
                      ISD::CTLZ,
                      ISD::CTTZ,
                      ISD::BSWAP,
                      ISD::SELECT,
                      ISD::SELECT_CC,
                      ISD::BR_CC,
                      ISD::SIGN_EXTEND_INREG,
                      ISD::BR_JT,
                      ISD::BRIND})
    setOperationAction(Op, MVT::i16, Expand);
  for (unsigned Op : {ISD::LOAD, ISD::STORE})
    setOperationAction(Op, MVT::i16, Custom);
  for (MVT VT : {MVT::i1, MVT::i8})
    for (auto Ext : {ISD::EXTLOAD, ISD::SEXTLOAD, ISD::ZEXTLOAD})
      setLoadExtAction(Ext, MVT::i16, VT, Expand);
  setTruncStoreAction(MVT::i16, MVT::i8, Expand);

  for (unsigned Op : {ISD::INTRINSIC_VOID, ISD::INTRINSIC_W_CHAIN})
    setOperationAction(Op, MVT::Other, Custom);

  for (unsigned Op : {ISD::GlobalAddress, ISD::ExternalSymbol,
                      ISD::ConstantPool, ISD::JumpTable, ISD::BlockAddress})
    setOperationAction(Op, MVT::i16, Custom);
}

EVT VUTargetLowering::getSetCCResultType(const DataLayout &DL,
                                         LLVMContext &Context, EVT VT) const {
  if (VT.isVector())
    return VT.changeVectorElementTypeToInteger();
  return MVT::i16;
}

SDValue VUTargetLowering::lowerUnsupported(SDValue Op, SelectionDAG &DAG,
                                           const char *What) const {
  SDLoc DL(Op);
  DAG.getContext()->diagnose(DiagnosticInfoUnsupported(
      DAG.getMachineFunction().getFunction(), What, DL.getDebugLoc()));
  // Keep the DAG well formed: replace every result by undef and keep the
  // chain.
  SmallVector<SDValue, 2> Results;
  for (unsigned I = 0, E = Op->getNumValues(); I != E; ++I) {
    EVT VT = Op->getValueType(I);
    Results.push_back(VT == MVT::Other ? Op.getOperand(0) : DAG.getUNDEF(VT));
  }
  return DAG.getMergeValues(Results, DL);
}

SDValue VUTargetLowering::lowerBUILD_VECTOR(SDValue Op,
                                            SelectionDAG &DAG) const {
  SDLoc DL(Op);
  auto *BV = cast<BuildVectorSDNode>(Op);

  // All zeros is matched directly.
  if (ISD::isBuildVectorAllZeros(Op.getNode()))
    return Op;

  if (SDValue Splat = BV->getSplatValue())
    return DAG.getNode(VUISD::SPLAT, DL, MVT::v4f32, Splat);

  // Insert the defined fields one by one.
  SDValue Vec = DAG.getUNDEF(MVT::v4f32);
  for (unsigned I = 0; I != 4; ++I) {
    SDValue Elt = Op.getOperand(I);
    if (Elt.isUndef())
      continue;
    Vec = DAG.getNode(ISD::INSERT_VECTOR_ELT, DL, MVT::v4f32, Vec, Elt,
                      DAG.getVectorIdxConstant(I, DL));
  }
  return Vec;
}

SDValue VUTargetLowering::lowerVectorField(SDValue Op,
                                           SelectionDAG &DAG) const {
  unsigned IdxOp = Op.getOpcode() == ISD::INSERT_VECTOR_ELT ? 2 : 1;
  if (isa<ConstantSDNode>(Op.getOperand(IdxOp)))
    return Op;
  return lowerUnsupported(Op, DAG, "VU: variable vector index");
}

SDValue VUTargetLowering::lowerIntrinsic(SDValue Op, SelectionDAG &DAG) const {
  // Operand 0 is the chain.
  switch (Op.getConstantOperandVal(1)) {
  case Intrinsic::vu_xgkick:
  case Intrinsic::vu_xtop:
    if (!Subtarget.hasVU1())
      return lowerUnsupported(Op, DAG, "VU: XGKICK and XTOP require VU1");
    break;
  default:
    break;
  }
  return Op;
}

void VUTargetLowering::getTgtMemIntrinsic(SmallVectorImpl<IntrinsicInfo> &Infos,
                                          const CallBase &I,
                                          MachineFunction &MF,
                                          unsigned Intrinsic) const {
  // The memory intrinsics access one quadword of VU data memory. The address
  // is not an IR pointer, so the memory operand has no value; the scheduler
  // and the pipeliner compare base registers and offsets instead.
  IntrinsicInfo Info;
  Info.memVT = MVT::v4f32;
  Info.align = Align(16);
  switch (Intrinsic) {
  default:
    return;
  case Intrinsic::vu_lq:
  case Intrinsic::vu_ilw:
    Info.opc = ISD::INTRINSIC_W_CHAIN;
    Info.flags = MachineMemOperand::MOLoad;
    break;
  case Intrinsic::vu_sq:
  case Intrinsic::vu_sq_mask:
  case Intrinsic::vu_isw:
    Info.opc = ISD::INTRINSIC_VOID;
    Info.flags = MachineMemOperand::MOStore;
    break;
  }
  Infos.push_back(Info);
}

SDValue VUTargetLowering::LowerOperation(SDValue Op, SelectionDAG &DAG) const {
  switch (Op.getOpcode()) {
  case ISD::BUILD_VECTOR:
    return lowerBUILD_VECTOR(Op, DAG);
  case ISD::SCALAR_TO_VECTOR:
    // The other fields are undefined, so a splat is fine.
    return DAG.getNode(VUISD::SPLAT, SDLoc(Op), MVT::v4f32, Op.getOperand(0));
  case ISD::INSERT_VECTOR_ELT:
  case ISD::EXTRACT_VECTOR_ELT:
    return lowerVectorField(Op, DAG);
  case ISD::INTRINSIC_VOID:
  case ISD::INTRINSIC_W_CHAIN:
    return lowerIntrinsic(Op, DAG);
  case ISD::LOAD:
  case ISD::STORE:
    return lowerUnsupported(Op, DAG,
                            "VU: use the llvm.vu.* intrinsics for memory "
                            "accesses");
  case ISD::GlobalAddress:
  case ISD::ExternalSymbol:
  case ISD::ConstantPool:
  case ISD::JumpTable:
  case ISD::BlockAddress:
    return lowerUnsupported(Op, DAG, "VU: global addresses are not supported");
  default:
    llvm_unreachable("unexpected custom lowering");
  }
}

SDValue VUTargetLowering::LowerFormalArguments(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &DL,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  if (!Ins.empty())
    DAG.getContext()->diagnose(DiagnosticInfoUnsupported(
        DAG.getMachineFunction().getFunction(),
        "VU: micro programs take no arguments", DL.getDebugLoc()));
  for (const ISD::InputArg &In : Ins)
    InVals.push_back(DAG.getUNDEF(In.VT));
  return Chain;
}

SDValue
VUTargetLowering::LowerReturn(SDValue Chain, CallingConv::ID CallConv,
                              bool IsVarArg,
                              const SmallVectorImpl<ISD::OutputArg> &Outs,
                              const SmallVectorImpl<SDValue> &OutVals,
                              const SDLoc &DL, SelectionDAG &DAG) const {
  if (!Outs.empty())
    DAG.getContext()->diagnose(DiagnosticInfoUnsupported(
        DAG.getMachineFunction().getFunction(),
        "VU: micro programs do not return a value", DL.getDebugLoc()));
  return DAG.getNode(VUISD::RET, DL, MVT::Other, Chain);
}

SDValue VUTargetLowering::LowerCall(CallLoweringInfo &CLI,
                                    SmallVectorImpl<SDValue> &InVals) const {
  SelectionDAG &DAG = CLI.DAG;
  DAG.getContext()->diagnose(DiagnosticInfoUnsupported(
      DAG.getMachineFunction().getFunction(),
      "VU: function calls are not supported", CLI.DL.getDebugLoc()));
  for (const ISD::InputArg &In : CLI.Ins)
    InVals.push_back(DAG.getUNDEF(In.VT));
  return CLI.Chain;
}
