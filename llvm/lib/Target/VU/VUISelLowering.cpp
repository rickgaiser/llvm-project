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
#include <cmath>

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
  for (unsigned Op : {ISD::MULHS,     ISD::MULHU,
                      ISD::SDIV,      ISD::UDIV,
                      ISD::SREM,      ISD::UREM,
                      ISD::SDIVREM,   ISD::UDIVREM,
                      ISD::SMUL_LOHI, ISD::UMUL_LOHI,
                      ISD::SRA,       ISD::SRL,
                      ISD::ROTL,      ISD::ROTR,
                      ISD::CTPOP,     ISD::CTLZ,
                      ISD::CTTZ,      ISD::BSWAP,
                      ISD::SELECT,    ISD::SELECT_CC,
                      ISD::BR_CC,     ISD::SIGN_EXTEND_INREG,
                      ISD::BR_JT,     ISD::BRIND})
    setOperationAction(Op, MVT::i16, Expand);
  for (unsigned Op : {ISD::LOAD, ISD::STORE, ISD::MUL, ISD::SHL})
    setOperationAction(Op, MVT::i16, Custom);
  for (MVT VT : {MVT::i1, MVT::i8})
    for (auto Ext : {ISD::EXTLOAD, ISD::SEXTLOAD, ISD::ZEXTLOAD})
      setLoadExtAction(Ext, MVT::i16, VT, Expand);
  setTruncStoreAction(MVT::i16, MVT::i8, Expand);

  for (unsigned Op : {ISD::INTRINSIC_VOID, ISD::INTRINSIC_W_CHAIN})
    setOperationAction(Op, MVT::Other, Custom);

  setTargetDAGCombine({ISD::FADD, ISD::FSUB, ISD::FMUL, ISD::FP_TO_SINT,
                       ISD::INSERT_VECTOR_ELT});

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

SDValue VUTargetLowering::lowerMUL(SDValue Op, SelectionDAG &DAG) const {
  // There is no integer multiplier and no shifter. A multiplication by a
  // constant is a sum of doublings of the other operand.
  SDLoc DL(Op);
  SDValue X = Op.getOperand(0), C = Op.getOperand(1);
  if (Op.getOpcode() == ISD::MUL && isa<ConstantSDNode>(X))
    std::swap(X, C);
  auto *CN = dyn_cast<ConstantSDNode>(C);
  if (!CN)
    return lowerUnsupported(Op, DAG,
                            Op.getOpcode() == ISD::MUL
                                ? "VU: integer multiply by a variable"
                                : "VU: shift by a variable");
  APInt M = CN->getAPIntValue().trunc(16);
  if (Op.getOpcode() == ISD::SHL)
    M = APInt::getOneBitSet(16, std::min<uint64_t>(CN->getZExtValue(), 15));
  // x * -c is 0 - x * c, when c has fewer bits set.
  bool Negate = M.isNegative() && (-M).popcount() < M.popcount();
  if (Negate)
    M = -M;
  SDValue Sum, Power = X;
  for (unsigned Bit = 0; !M.lshr(Bit).isZero(); ++Bit) {
    if (M[Bit])
      Sum = Sum ? DAG.getNode(ISD::ADD, DL, MVT::i16, Sum, Power) : Power;
    if (!M.lshr(Bit + 1).isZero())
      Power = DAG.getNode(ISD::ADD, DL, MVT::i16, Power, Power);
  }
  SDValue Zero = DAG.getConstant(0, DL, MVT::i16);
  if (!Sum)
    return Zero;
  return Negate ? DAG.getNode(ISD::SUB, DL, MVT::i16, Zero, Sum) : Sum;
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
  case ISD::MUL:
  case ISD::SHL:
    return lowerMUL(Op, DAG);
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

/// Returns true if \p V broadcasts one field of a VF register, and sets
/// \p Src and \p Field.
static bool isBroadcast(SDValue V, SDValue &Src, unsigned &Field) {
  if (V.getOpcode() == VUISD::BCAST) {
    Src = V.getOperand(0);
    Field = V.getConstantOperandVal(1);
    return true;
  }
  if (V.getOpcode() != VUISD::SPLAT)
    return false;
  SDValue S = V.getOperand(0);
  if (S.getOpcode() == ISD::EXTRACT_VECTOR_ELT &&
      isa<ConstantSDNode>(S.getOperand(1))) {
    Src = S.getOperand(0);
    Field = S.getConstantOperandVal(1);
  } else {
    Src = S;
    Field = 0;
  }
  return true;
}

namespace {
/// A term a * b of a sum computed through ACC. Field selects a broadcast
/// field of b (0-3), or all fields (VUISD::AccSumFull).
struct AccTerm {
  SDValue A, B;
  unsigned Field;
  bool Neg;
};
} // namespace

static bool isAccChain(SDValue V) {
  return (V.getOpcode() == ISD::FADD || V.getOpcode() == ISD::FSUB) &&
         V.getValueType() == MVT::v4f32 && V.hasOneUse();
}

static void addAccLeaf(SDValue V, bool Neg, SelectionDAG &DAG,
                       SmallVectorImpl<AccTerm> &Terms) {
  if (V.getOpcode() == ISD::FMUL && V.hasOneUse()) {
    SDValue X = V.getOperand(0), Y = V.getOperand(1), Src;
    unsigned Field;
    if (isBroadcast(Y, Src, Field))
      Terms.push_back({X, Src, Field, Neg});
    else if (isBroadcast(X, Src, Field))
      Terms.push_back({Y, Src, Field, Neg});
    else
      Terms.push_back({X, Y, VUISD::AccSumFull, Neg});
    return;
  }
  // c = c * vf0.w
  Terms.push_back({V, DAG.getRegister(VU::VF0, MVT::v4f32), 3, Neg});
}

/// Collects the terms of a left-linear chain of FADD and FSUB nodes, in
/// evaluation order.
static void collectAccTerms(SDValue V, SelectionDAG &DAG,
                            SmallVectorImpl<AccTerm> &Terms) {
  SDValue L = V.getOperand(0), R = V.getOperand(1);
  if (V.getOpcode() == ISD::FADD && !isAccChain(L) && isAccChain(R))
    std::swap(L, R);
  if (isAccChain(L))
    collectAccTerms(L, DAG, Terms);
  else
    addAccLeaf(L, /*Neg=*/false, DAG, Terms);
  addAccLeaf(R, V.getOpcode() == ISD::FSUB, DAG, Terms);
}

// Computes a sum of products as MULA, MADDA..., MADD, which forwards the
// accumulator between the steps:
//   (m0 * v.x + m1 * v.y) + m2 * v.z + m3
//   -> MULAx acc, m0, v; MADDAy acc, m1, v; MADDAz acc, m2, v;
//      MADDw d, m3, vf0
// The terms are accumulated in the original order, so the result is the
// same.
SDValue VUTargetLowering::combineAccSum(SDNode *N, SelectionDAG &DAG) const {
  if (N->getValueType(0) != MVT::v4f32)
    return SDValue();
  // Only the root of a chain.
  if (N->hasOneUse() && isAccChain(SDValue(*N->user_begin(), 0)))
    return SDValue();

  SmallVector<AccTerm, 8> Terms;
  collectAccTerms(SDValue(N, 0), DAG, Terms);
  // MULA can not negate; it is only worth it with a product.
  if (Terms[0].Neg || none_of(Terms, [](const AccTerm &T) {
        return T.B.getOpcode() != ISD::Register;
      }))
    return SDValue();

  SDLoc DL(N);
  SmallVector<SDValue, 16> Ops;
  for (const AccTerm &T : Terms) {
    Ops.push_back(T.A);
    Ops.push_back(T.B);
    Ops.push_back(DAG.getTargetConstant(T.Field, DL, MVT::i32));
    Ops.push_back(DAG.getTargetConstant(T.Neg, DL, MVT::i32));
  }
  return DAG.getNode(VUISD::ACCSUM, DL, MVT::v4f32, Ops);
}

/// Returns n if \p C is a splat of 2^n, or of 2^-n if \p Inverse, for the
/// fraction bits of the fixed-point conversions, or 0.
static unsigned getFractionBits(SDValue C, bool Inverse) {
  ConstantFPSDNode *CN = isConstOrConstSplatFP(C);
  if (!CN)
    return 0;
  for (int N : {4, 12, 15})
    if (CN->getValueAPF().isExactlyValue(std::ldexp(1.0, Inverse ? -N : N)))
      return N;
  return 0;
}

SDValue VUTargetLowering::PerformDAGCombine(SDNode *N,
                                            DAGCombinerInfo &DCI) const {
  SelectionDAG &DAG = DCI.DAG;
  SDLoc DL(N);
  switch (N->getOpcode()) {
  case ISD::FP_TO_SINT: {
    // fp_to_sint (v * 2^n) -> FTOI<n> v. The multiplication is exact.
    SDValue Mul = N->getOperand(0);
    if (N->getValueType(0) != MVT::v4i32 || Mul.getOpcode() != ISD::FMUL ||
        !Mul.hasOneUse())
      break;
    for (unsigned I : {0, 1})
      if (unsigned Bits = getFractionBits(Mul.getOperand(1 - I), false))
        return DAG.getNode(VUISD::FTOI, DL, MVT::v4i32, Mul.getOperand(I),
                           DAG.getTargetConstant(Bits, DL, MVT::i32));
    break;
  }
  case ISD::FMUL: {
    // sint_to_fp (v) * 2^-n -> ITOF<n> v
    if (N->getValueType(0) != MVT::v4f32)
      break;
    for (unsigned I : {0, 1}) {
      SDValue Conv = N->getOperand(I);
      if (Conv.getOpcode() != ISD::SINT_TO_FP ||
          Conv.getOperand(0).getValueType() != MVT::v4i32)
        continue;
      if (unsigned Bits = getFractionBits(N->getOperand(1 - I), true))
        return DAG.getNode(VUISD::ITOF, DL, MVT::v4f32, Conv.getOperand(0),
                           DAG.getTargetConstant(Bits, DL, MVT::i32));
    }
    break;
  }
  case ISD::INSERT_VECTOR_ELT: {
    // A 16-bit integer inserted into an integer vector: MFIR sign-extends.
    // This is done before the i32 element is legalized.
    SDValue Elt = N->getOperand(1);
    auto *Idx = dyn_cast<ConstantSDNode>(N->getOperand(2));
    if (N->getValueType(0) != MVT::v4i32 || !Idx || Idx->getZExtValue() > 3 ||
        (Elt.getOpcode() != ISD::SIGN_EXTEND &&
         Elt.getOpcode() != ISD::ANY_EXTEND) ||
        Elt.getOperand(0).getValueType() != MVT::i16)
      break;
    return DAG.getNode(
        VUISD::INSERT_VI, DL, MVT::v4i32, N->getOperand(0), Elt.getOperand(0),
        DAG.getTargetConstant(Idx->getZExtValue(), DL, MVT::i32));
  }
  case VUISD::SPLAT: {
    // Broadcast straight from the field: no move to the x field first.
    SDValue Src;
    unsigned Field;
    isBroadcast(SDValue(N, 0), Src, Field);
    return DAG.getNode(VUISD::BCAST, DL, MVT::v4f32, Src,
                       DAG.getTargetConstant(Field, DL, MVT::i32));
  }
  case ISD::FADD:
  case ISD::FSUB:
    if (DCI.isAfterLegalizeDAG())
      return combineAccSum(N, DAG);
    break;
  }
  return SDValue();
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
