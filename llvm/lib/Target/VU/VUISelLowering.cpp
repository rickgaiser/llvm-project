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
#include "llvm/CodeGen/SelectionDAGSums.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IntrinsicsVU.h"
#include "llvm/Support/CommandLine.h"
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
    for (unsigned Op :
         {ISD::BITCAST, ISD::SINT_TO_FP, ISD::FP_TO_SINT, ISD::UNDEF})
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
  // MAX and MINI on the x field. The VU has no NaNs.
  for (unsigned Op : {ISD::FMAXNUM, ISD::FMINNUM})
    setOperationAction(Op, MVT::f32, Legal);
  for (unsigned Op : {ISD::BUILD_VECTOR, ISD::SCALAR_TO_VECTOR,
                      ISD::INSERT_VECTOR_ELT, ISD::EXTRACT_VECTOR_ELT})
    setOperationAction(Op, MVT::v4f32, Custom);

  // Scalar floats.
  for (unsigned Op :
       {ISD::FREM, ISD::FSINCOS, ISD::FCOPYSIGN, ISD::FRINT, ISD::FNEARBYINT,
        ISD::FMINIMUM, ISD::FMAXIMUM, ISD::SELECT_CC, ISD::BR_CC})
    setOperationAction(Op, MVT::f32, Expand);
  // Rounding and the math functions from FMAC operations, and divisions for
  // some (see lowerMath). The EFU is not used: it is VU1 only, valid in a
  // part of the range only, and not pipelined.
  for (MVT VT : {MVT::f32, MVT::v4f32})
    for (unsigned Op :
         {ISD::FFLOOR, ISD::FCEIL, ISD::FTRUNC, ISD::FROUND, ISD::FSIN,
          ISD::FCOS, ISD::FTAN, ISD::FEXP, ISD::FEXP2, ISD::FEXP10, ISD::FLOG,
          ISD::FLOG2, ISD::FLOG10, ISD::FPOW, ISD::FATAN, ISD::FATAN2,
          ISD::FASIN, ISD::FACOS})
      setOperationAction(Op, VT, Custom);
  for (unsigned Op : {ISD::LOAD, ISD::STORE})
    setOperationAction(Op, MVT::f32, Custom);

  // Comparisons are masks of 0.0 and 1.0, and selects sums of products with
  // them (see getMask).
  setBooleanVectorContents(ZeroOrOneBooleanContent);
  for (MVT VT : {MVT::f32, MVT::v4f32})
    for (unsigned Op : {ISD::SETCC, ISD::SELECT})
      setOperationAction(Op, VT, Custom);
  setOperationAction(ISD::VSELECT, MVT::v4f32, Custom);
  setOperationAction(ISD::BRCOND, MVT::Other, Custom);
  for (MVT VT : {MVT::i16, MVT::v4i32})
    setOperationAction(ISD::UINT_TO_FP, VT, Custom);

  // 16-bit integers.
  for (unsigned Op : {ISD::MULHS,     ISD::MULHU,   ISD::SDIV,
                      ISD::UDIV,      ISD::SREM,    ISD::UREM,
                      ISD::SDIVREM,   ISD::UDIVREM, ISD::SMUL_LOHI,
                      ISD::UMUL_LOHI, ISD::SRA,     ISD::SRL,
                      ISD::ROTL,      ISD::ROTR,    ISD::CTPOP,
                      ISD::CTLZ,      ISD::CTTZ,    ISD::BSWAP,
                      ISD::SELECT_CC, ISD::BR_CC,   ISD::SIGN_EXTEND_INREG,
                      ISD::BR_JT,     ISD::BRIND})
    setOperationAction(Op, MVT::i16, Expand);
  for (unsigned Op :
       {ISD::LOAD, ISD::STORE, ISD::MUL, ISD::SHL, ISD::XOR, ISD::SELECT})
    setOperationAction(Op, MVT::i16, Custom);
  for (MVT VT : {MVT::i1, MVT::i8})
    for (auto Ext : {ISD::EXTLOAD, ISD::SEXTLOAD, ISD::ZEXTLOAD})
      setLoadExtAction(Ext, MVT::i16, VT, Expand);
  setTruncStoreAction(MVT::i16, MVT::i8, Expand);

  for (unsigned Op : {ISD::INTRINSIC_VOID, ISD::INTRINSIC_W_CHAIN})
    setOperationAction(Op, MVT::Other, Custom);

  setTargetDAGCombine({ISD::FADD, ISD::FSUB, ISD::FMUL, ISD::FMAXNUM,
                       ISD::FMINNUM, ISD::FP_TO_SINT, ISD::INSERT_VECTOR_ELT,
                       ISD::EXTRACT_VECTOR_ELT, ISD::SINT_TO_FP,
                       ISD::UINT_TO_FP, ISD::SELECT, ISD::VSELECT,
                       ISD::BUILD_VECTOR, ISD::FDIV});

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

static SDValue getBroadcast(SDValue V, unsigned Field, const SDLoc &DL,
                            SelectionDAG &DAG) {
  return DAG.getNode(VUISD::BCAST, DL, MVT::v4f32, V,
                     DAG.getTargetConstant(Field, DL, MVT::i32));
}

/// 1.0 in every field: vf0.w.
static SDValue getOnes(const SDLoc &DL, SelectionDAG &DAG) {
  return getBroadcast(DAG.getRegister(VU::VF0, MVT::v4f32), 3, DL, DAG);
}

/// 1.0 in the fields where \p D > 0, 0.0 in the others: D * 2^127, clamped
/// to 0 ... 1. The VU has no denormals and saturates instead of overflowing,
/// so every D > 0 (at least 2^-126) gives at least 2.0.
static SDValue getPositiveMask(SDValue D, const SDLoc &DL, SelectionDAG &DAG) {
  SDValue Big = getBroadcast(
      DAG.getConstantFP(std::ldexp(1.0, 127), DL, MVT::f32), 0, DL, DAG);
  SDValue M = DAG.getNode(ISD::FMUL, DL, MVT::v4f32, D, Big);
  M = DAG.getNode(
      ISD::FMAXNUM, DL, MVT::v4f32, M,
      getBroadcast(DAG.getRegister(VU::VF0, MVT::v4f32), 0, DL, DAG));
  return DAG.getNode(ISD::FMINNUM, DL, MVT::v4f32, M, getOnes(DL, DAG));
}

static bool isOnes(SDValue V) {
  auto *R = V.getOpcode() == VUISD::BCAST
                ? dyn_cast<RegisterSDNode>(V.getOperand(0))
                : nullptr;
  return R && R->getReg() == VU::VF0 && V.getConstantOperandVal(1) == 3;
}

/// 1 - \p M for a mask.
static SDValue invertMask(SDValue M, const SDLoc &DL, SelectionDAG &DAG) {
  if (M.getOpcode() == ISD::FSUB && isOnes(M.getOperand(0)))
    return M.getOperand(1);
  return DAG.getNode(ISD::FSUB, DL, MVT::v4f32, getOnes(DL, DAG), M);
}

/// The operands of a scalar comparison as vectors with the scalars in field
/// \p Field: a field of a vector is used in its place, other values are
/// broadcast unless the field is x.
static SDValue getScalarAsVector(SDValue S, unsigned Field, const SDLoc &DL,
                                 SelectionDAG &DAG) {
  if (S.getOpcode() == ISD::EXTRACT_VECTOR_ELT &&
      isa<ConstantSDNode>(S.getOperand(1)) &&
      S.getConstantOperandVal(1) == Field)
    return S.getOperand(0);
  if (Field == 0)
    return DAG.getNode(VUISD::VEC, DL, MVT::v4f32, S);
  return getBroadcast(S, 0, DL, DAG);
}

/// Returns the field of a vector that \p S is, or 0.
static unsigned getScalarField(SDValue S) {
  if (S.getOpcode() == ISD::EXTRACT_VECTOR_ELT &&
      isa<ConstantSDNode>(S.getOperand(1)))
    return S.getConstantOperandVal(1);
  return 0;
}

static bool isZeroMask(SDValue V) {
  return isNullFPConstant(V) || ISD::isBuildVectorAllZeros(V.getNode());
}

static bool isMaskOne(SDValue V) {
  return isOneOrOneSplat(V) || isAllOnesOrAllOnesSplat(V);
}

// There are no comparison instructions that give a value, and no selects.
// A comparison is a mask instead, 1.0 where it is true and 0.0 elsewhere:
//   a > b   ->  clamp((a - b) * 2^127, 0, 1)
//   a >= b  ->  1 - (b > a)
//   a == b  ->  1 - (|a - b| > 0)
// and a select a sum of products, exact with 0.0 and 1.0 (see BLEND):
//   select c, x, y  ->  x * m + y * (1 - m)
// The VU has no NaNs, so ordered and unordered comparisons are the same. The
// mask is valid in \p Field (0-3), or in all fields for VUISD::AccSumFull.
// A difference below 2^-126 is 0 on the VU, so values that close compare
// equal.
std::pair<SDValue, unsigned>
VUTargetLowering::getMask(SDValue C, SelectionDAG &DAG) const {
  SDLoc DL(C);
  switch (C.getOpcode()) {
  case VUISD::MASK_INT:
    return {C.getOperand(0), unsigned(C.getConstantOperandVal(1))};
  case ISD::SETCC: {
    SDValue A = C.getOperand(0), B = C.getOperand(1);
    EVT VT = A.getValueType();
    unsigned Field = VUISD::AccSumFull;
    if (VT == MVT::f32) {
      Field = getScalarField(A);
      if (!Field)
        Field = getScalarField(B);
      // 0.0 stays a constant: a comparison with it needs no subtraction.
      for (SDValue *V : {&A, &B})
        *V = isZeroMask(*V) ? DAG.getConstantFP(0.0, DL, MVT::v4f32)
                            : getScalarAsVector(*V, Field, DL, DAG);
    } else if (VT != MVT::v4f32) {
      return {};
    }
    auto Diff = [&](SDValue X, SDValue Y) {
      if (isZeroMask(Y))
        return X;
      return DAG.getNode(ISD::FSUB, DL, MVT::v4f32, X, Y);
    };
    auto Abs = [&](SDValue X) {
      return DAG.getNode(ISD::FABS, DL, MVT::v4f32, X);
    };
    SDValue M;
    switch (cast<CondCodeSDNode>(C.getOperand(2))->get()) {
    case ISD::SETOGT:
    case ISD::SETUGT:
    case ISD::SETGT:
      M = getPositiveMask(Diff(A, B), DL, DAG);
      break;
    case ISD::SETOLT:
    case ISD::SETULT:
    case ISD::SETLT:
      M = getPositiveMask(Diff(B, A), DL, DAG);
      break;
    case ISD::SETOGE:
    case ISD::SETUGE:
    case ISD::SETGE:
      M = invertMask(getPositiveMask(Diff(B, A), DL, DAG), DL, DAG);
      break;
    case ISD::SETOLE:
    case ISD::SETULE:
    case ISD::SETLE:
      M = invertMask(getPositiveMask(Diff(A, B), DL, DAG), DL, DAG);
      break;
    case ISD::SETOEQ:
    case ISD::SETUEQ:
    case ISD::SETEQ:
      M = invertMask(getPositiveMask(Abs(Diff(A, B)), DL, DAG), DL, DAG);
      break;
    case ISD::SETONE:
    case ISD::SETUNE:
    case ISD::SETNE:
      M = getPositiveMask(Abs(Diff(A, B)), DL, DAG);
      break;
    case ISD::SETO:
    case ISD::SETTRUE:
    case ISD::SETTRUE2:
      M = getOnes(DL, DAG);
      break;
    case ISD::SETUO:
    case ISD::SETFALSE:
    case ISD::SETFALSE2:
      M = DAG.getConstantFP(0.0, DL, MVT::v4f32);
      break;
    default:
      return {};
    }
    return {M, Field};
  }
  case ISD::AND:
  case ISD::OR:
  case ISD::XOR: {
    auto [L, LField] = getMask(C.getOperand(0), DAG);
    if (!L)
      return {};
    // c & 1 from the promotion of i1, and c ^ 1, which is not c.
    if (isMaskOne(C.getOperand(1))) {
      if (C.getOpcode() == ISD::AND)
        return {L, LField};
      if (C.getOpcode() == ISD::XOR)
        return {invertMask(L, DL, DAG), LField};
      return {};
    }
    auto [R, RField] = getMask(C.getOperand(1), DAG);
    if (!R)
      return {};
    // The masks of scalars may be in different fields: the second one is
    // read by its broadcast field.
    if (RField != LField) {
      if (LField == VUISD::AccSumFull || RField == VUISD::AccSumFull)
        return {};
      R = getBroadcast(R, RField, DL, DAG);
    }
    switch (C.getOpcode()) {
    case ISD::AND:
      return {DAG.getNode(ISD::FMUL, DL, MVT::v4f32, L, R), LField};
    case ISD::OR:
      return {DAG.getNode(ISD::FMAXNUM, DL, MVT::v4f32, L, R), LField};
    default:
      return {DAG.getNode(ISD::FABS, DL, MVT::v4f32,
                          DAG.getNode(ISD::FSUB, DL, MVT::v4f32, L, R)),
              LField};
    }
  }
  }
  return {};
}

SDValue VUTargetLowering::lowerSETCC(SDValue Op, SelectionDAG &DAG) const {
  auto [M, Field] = getMask(Op, DAG);
  if (!M)
    return lowerUnsupported(Op, DAG, "VU: comparison");
  SDLoc DL(Op);
  return DAG.getNode(VUISD::MASK_INT, DL, Op.getValueType(), M,
                     DAG.getTargetConstant(Field, DL, MVT::i32));
}

SDValue VUTargetLowering::lowerSELECT(SDValue Op, SelectionDAG &DAG) const {
  SDLoc DL(Op);
  SDValue C = Op.getOperand(0), X = Op.getOperand(1), Y = Op.getOperand(2);
  EVT VT = Op.getValueType();
  if (VT == MVT::i16) {
    // c is 0 or 1: y + ((x - y) & -c)
    SDValue Zero = DAG.getConstant(0, DL, MVT::i16);
    SDValue C16 = DAG.getZExtOrTrunc(C, DL, MVT::i16);
    SDValue D = DAG.getNode(ISD::SUB, DL, MVT::i16, X, Y);
    D = DAG.getNode(ISD::AND, DL, MVT::i16, D,
                    DAG.getNode(ISD::SUB, DL, MVT::i16, Zero, C16));
    return DAG.getNode(ISD::ADD, DL, MVT::i16, Y, D);
  }
  auto [M, Field] = getMask(C, DAG);
  if (!M) {
    // Any other condition is 0 or 1.
    if (C.getValueType().isVector()) {
      M = DAG.getNode(ISD::SINT_TO_FP, DL, MVT::v4f32, C);
      Field = VUISD::AccSumFull;
    } else {
      SDValue F = DAG.getNode(ISD::SINT_TO_FP, DL, MVT::f32,
                              DAG.getZExtOrTrunc(C, DL, MVT::i16));
      M = DAG.getNode(VUISD::VEC, DL, MVT::v4f32, F);
      Field = 0;
    }
  }
  // A select of a vector by a scalar condition reads the mask by its
  // broadcast field.
  if (VT == MVT::v4f32 && Field != VUISD::AccSumFull &&
      C.getValueType().isVector())
    return lowerUnsupported(Op, DAG, "VU: select");
  SDValue Res;
  ConstantFPSDNode *CX = isConstOrConstSplatFP(X),
                   *CY = isConstOrConstSplatFP(Y);
  if (CX && CY && CX->isExactlyValue(1.0) && CY->isZero())
    Res = Field == VUISD::AccSumFull ? M : getBroadcast(M, Field, DL, DAG);
  else if (CX && CY && CX->isZero() && CY->isExactlyValue(1.0))
    Res = invertMask(
        Field == VUISD::AccSumFull ? M : getBroadcast(M, Field, DL, DAG), DL,
        DAG);
  // A select of 0.0 is a product with the mask, in ft. A constant would be
  // moved to ft by the DAG combiner.
  auto ByMask = [&](SDValue V, SDValue Mask) {
    if (VT == MVT::f32)
      V = DAG.getNode(VUISD::VEC, DL, MVT::v4f32, V);
    if (Field != VUISD::AccSumFull)
      Mask = getBroadcast(Mask, Field, DL, DAG);
    return DAG.getNode(ISD::FMUL, DL, MVT::v4f32, V, Mask);
  };
  if (!Res && isZeroMask(Y) && !CX)
    Res = ByMask(X, M);
  if (!Res && isZeroMask(X) && !CY)
    Res = ByMask(Y, invertMask(M, DL, DAG));
  if (!Res) {
    if (VT == MVT::f32) {
      X = DAG.getNode(VUISD::VEC, DL, MVT::v4f32, X);
      Y = DAG.getNode(VUISD::VEC, DL, MVT::v4f32, Y);
    }
    Res = DAG.getNode(VUISD::BLEND, DL, MVT::v4f32, X, M, Y,
                      invertMask(M, DL, DAG),
                      DAG.getTargetConstant(Field, DL, MVT::i32));
  }
  if (VT == MVT::f32)
    return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, MVT::f32, Res,
                       DAG.getVectorIdxConstant(0, DL));
  return Res;
}

SDValue VUTargetLowering::lowerUINT_TO_FP(SDValue Op, SelectionDAG &DAG) const {
  SDLoc DL(Op);
  SDValue V = Op.getOperand(0);
  EVT VT = Op.getValueType();
  if (auto [M, Field] = getMask(V, DAG); M) {
    if (VT == MVT::v4f32)
      return M;
    return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, MVT::f32, M,
                       DAG.getVectorIdxConstant(Field, DL));
  }
  if (DAG.SignBitIsZero(V))
    return DAG.getNode(ISD::SINT_TO_FP, DL, VT, V);
  // A negative i16 is 2^16 more, a negative i32 2^32: f + 2^n * (f < 0).
  bool Vector = VT == MVT::v4f32;
  SDValue F = DAG.getNode(ISD::SINT_TO_FP, DL, VT, V);
  if (!Vector)
    F = DAG.getNode(VUISD::VEC, DL, MVT::v4f32, F);
  SDValue Neg =
      getPositiveMask(DAG.getNode(ISD::FSUB, DL, MVT::v4f32,
                                  DAG.getConstantFP(0.0, DL, MVT::v4f32), F),
                      DL, DAG);
  SDValue Res = DAG.getNode(
      ISD::FADD, DL, MVT::v4f32, F,
      DAG.getNode(
          ISD::FMUL, DL, MVT::v4f32, Neg,
          getBroadcast(
              DAG.getConstantFP(Vector ? 4294967296.0 : 65536.0, DL, MVT::f32),
              0, DL, DAG)));
  if (Vector)
    return Res;
  return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, MVT::f32, Res,
                     DAG.getVectorIdxConstant(0, DL));
}

/// floor(v) for |v| < 2^22. Adding and subtracting 1.5 * 2^23 drops the
/// fraction of the positive sum (the VU rounds towards zero), but the PS2's
/// VU cuts the smaller operand of an addition to one bit below the last bit
/// of the sum first (PCSX2 does not): for a negative v with a fraction below
/// 0.5, t = (v + c) - c is floor(v) + 1. It is never below floor(v), so one
/// is taken off where t > v. (The nodes have no fast-math flags: (v + c) - c
/// must not be folded to v.)
static SDValue getFloor(SDValue V, const SDLoc &DL, SelectionDAG &DAG) {
  SDValue C =
      getBroadcast(DAG.getConstantFP(12582912.0, DL, MVT::f32), 0, DL, DAG);
  SDValue T = DAG.getNode(ISD::FSUB, DL, MVT::v4f32,
                          DAG.getNode(ISD::FADD, DL, MVT::v4f32, V, C), C);
  SDValue Over =
      getPositiveMask(DAG.getNode(ISD::FSUB, DL, MVT::v4f32, T, V), DL, DAG);
  return DAG.getNode(ISD::FSUB, DL, MVT::v4f32, T, Over);
}

static SDValue getNeg(SDValue V, const SDLoc &DL, SelectionDAG &DAG) {
  return DAG.getNode(ISD::FSUB, DL, MVT::v4f32,
                     DAG.getConstantFP(0.0, DL, MVT::v4f32), V);
}

/// sin(v): u = v / 2pi - round(v / 2pi) in [-0.5, 0.5), then an odd
/// polynomial in u of degree 9 (error 6e-6). ESIN takes 28 cycles (measured),
/// is valid in [-pi/2, pi/2] only and is not pipelined; this is FMAC work only.
static SDValue getSin(SDValue V, const SDLoc &DL, SelectionDAG &DAG) {
  auto Const = [&](double C) {
    return getBroadcast(DAG.getConstantFP(C, DL, MVT::f32), 0, DL, DAG);
  };
  SDValue T =
      DAG.getNode(ISD::FMUL, DL, MVT::v4f32, V, Const(0.15915494309189535));
  SDValue U = DAG.getNode(
      ISD::FSUB, DL, MVT::v4f32, T,
      getFloor(DAG.getNode(ISD::FADD, DL, MVT::v4f32, T, Const(0.5)), DL, DAG));
  SDValue U2 = DAG.getNode(ISD::FMUL, DL, MVT::v4f32, U, U);
  static constexpr double Coeffs[] = {6.283055791, -41.33121387, 81.36680843,
                                      -74.4779649, 32.78132493};
  SDValue Y = DAG.getNode(ISD::FMUL, DL, MVT::v4f32, U2, Const(Coeffs[4]));
  for (int I = 3; I >= 0; --I) {
    Y = DAG.getNode(ISD::FADD, DL, MVT::v4f32, Y, Const(Coeffs[I]));
    Y = DAG.getNode(ISD::FMUL, DL, MVT::v4f32, Y, I ? U2 : U);
  }
  return Y;
}

static SDValue getConst(double C, const SDLoc &DL, SelectionDAG &DAG) {
  return getBroadcast(DAG.getConstantFP(C, DL, MVT::f32), 0, DL, DAG);
}

/// The polynomial with \p Coeffs (constant first) at \p X, by Horner's rule.
static SDValue getPolynomial(SDValue X, ArrayRef<double> Coeffs,
                             const SDLoc &DL, SelectionDAG &DAG) {
  SDValue Y = DAG.getNode(ISD::FMUL, DL, MVT::v4f32, X,
                          getConst(Coeffs.back(), DL, DAG));
  for (int I = Coeffs.size() - 2; I >= 0; --I) {
    Y = DAG.getNode(ISD::FADD, DL, MVT::v4f32, Y, getConst(Coeffs[I], DL, DAG));
    if (I)
      Y = DAG.getNode(ISD::FMUL, DL, MVT::v4f32, Y, X);
  }
  return Y;
}

/// 2^n for integers n in -126 ... 127: the bits of the float are
/// (n + 127) << 23, which FTOI0 makes from the float (n + 127) * 2^23.
static SDValue getPow2(SDValue N, const SDLoc &DL, SelectionDAG &DAG) {
  SDValue E = DAG.getNode(ISD::FADD, DL, MVT::v4f32, N, getConst(127, DL, DAG));
  E = DAG.getNode(ISD::FMUL, DL, MVT::v4f32, E,
                  getConst(std::ldexp(1.0, 23), DL, DAG));
  return DAG.getNode(ISD::BITCAST, DL, MVT::v4f32,
                     DAG.getNode(ISD::FP_TO_SINT, DL, MVT::v4i32, E));
}

/// 2^x: 2^floor(x) * p(x - floor(x)), p of degree 6 (error 3e-7). x is
/// clamped to -126 ... 128: the VU has no denormals and no infinity.
static SDValue getExp2(SDValue X, const SDLoc &DL, SelectionDAG &DAG) {
  X = DAG.getNode(ISD::FMAXNUM, DL, MVT::v4f32, X, getConst(-126, DL, DAG));
  X = DAG.getNode(ISD::FMINNUM, DL, MVT::v4f32, X,
                  getConst(127.99999, DL, DAG));
  SDValue N = getFloor(X, DL, DAG);
  SDValue F = DAG.getNode(ISD::FSUB, DL, MVT::v4f32, X, N);
  static constexpr double Coeffs[] = {
      1.0000000018154942,   0.6931469870574731,   0.24022979581423864,
      0.0554835247240163,   0.009678472554336515, 0.0012443081509537213,
      0.0002169060916594745};
  // The power of two in ft: exact on the PS2.
  return DAG.getNode(ISD::FMUL, DL, MVT::v4f32,
                     getPolynomial(F, Coeffs, DL, DAG), getPow2(N, DL, DAG));
}

/// log2(x) for x > 0: x = m * 2^e with m in [1, 2), then e + p(m - 1), p of
/// degree 7 (error 5e-7). e + 127 is the integer part of the bits of x
/// divided by 2^23.
static SDValue getLog2(SDValue X, const SDLoc &DL, SelectionDAG &DAG) {
  SDValue Bits = DAG.getNode(ISD::SINT_TO_FP, DL, MVT::v4f32,
                             DAG.getNode(ISD::BITCAST, DL, MVT::v4i32, X));
  // e + 127; it may be one more where m is just below 2, as ITOF0 rounds:
  // then m is just below 1, which p handles too.
  SDValue E = getFloor(DAG.getNode(ISD::FMUL, DL, MVT::v4f32, Bits,
                                   getConst(std::ldexp(1.0, -23), DL, DAG)),
                       DL, DAG);
  // m = x * 2^-e, the power of two in ft.
  SDValue M = DAG.getNode(
      ISD::FMUL, DL, MVT::v4f32, X,
      getPow2(DAG.getNode(ISD::FSUB, DL, MVT::v4f32, getConst(127, DL, DAG), E),
              DL, DAG));
  SDValue T = DAG.getNode(ISD::FSUB, DL, MVT::v4f32, M, getOnes(DL, DAG));
  static constexpr double Coeffs[] = {0.0,
                                      1.4426640464277907,
                                      -0.7205155141084034,
                                      0.4731133450222758,
                                      -0.32461638360746325,
                                      0.19238505485303956,
                                      -0.07815820827399941,
                                      0.015127916047827428};
  // The constant 0 is left out: t * (c1 + t * (...)).
  SDValue P = getPolynomial(T, ArrayRef(Coeffs).drop_front(), DL, DAG);
  P = DAG.getNode(ISD::FMUL, DL, MVT::v4f32, P, T);
  return DAG.getNode(
      ISD::FADD, DL, MVT::v4f32, P,
      DAG.getNode(ISD::FSUB, DL, MVT::v4f32, E, getConst(127, DL, DAG)));
}

/// a / b per field with DIV, in field \p Field only for a scalar (4: all).
static SDValue getDivide(SDValue A, SDValue B, unsigned Field, const SDLoc &DL,
                         SelectionDAG &DAG) {
  SDValue Res = DAG.getUNDEF(MVT::v4f32);
  for (unsigned I = 0; I != 4; ++I) {
    if (Field != 4 && I != Field)
      continue;
    auto F = [&](SDValue V) {
      return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, MVT::f32, V,
                         DAG.getVectorIdxConstant(I, DL));
    };
    Res = DAG.getNode(ISD::INSERT_VECTOR_ELT, DL, MVT::v4f32, Res,
                      DAG.getNode(ISD::FDIV, DL, MVT::f32, F(A), F(B)),
                      DAG.getVectorIdxConstant(I, DL));
  }
  return Res;
}

/// atan(x): for |x| > 1, pi/2 - atan(1 / |x|), then an odd polynomial of
/// degree 13 (error 5e-7), and the sign of x.
static SDValue getAtan(SDValue X, unsigned Field, const SDLoc &DL,
                       SelectionDAG &DAG) {
  SDValue A = DAG.getNode(ISD::FABS, DL, MVT::v4f32, X);
  SDValue Big = getPositiveMask(
      DAG.getNode(ISD::FSUB, DL, MVT::v4f32, A, getOnes(DL, DAG)), DL, DAG);
  // 1 / |x| is not used where |x| <= 1; it is the largest float for 0.
  SDValue R =
      DAG.getNode(VUISD::BLEND, DL, MVT::v4f32,
                  getDivide(getOnes(DL, DAG), A, Field, DL, DAG), Big, A,
                  invertMask(Big, DL, DAG),
                  DAG.getTargetConstant(VUISD::AccSumFull, DL, MVT::i32));
  SDValue R2 = DAG.getNode(ISD::FMUL, DL, MVT::v4f32, R, R);
  static constexpr double Coeffs[] = {0.999996634700669,   -0.33318302899445745,
                                      0.1981321350906017,  -0.13247522771595718,
                                      0.07981120495557731, -0.03372593810361328,
                                      0.006842624897388613};
  SDValue P = DAG.getNode(ISD::FMUL, DL, MVT::v4f32,
                          getPolynomial(R2, Coeffs, DL, DAG), R);
  // pi/2 - p where |x| > 1: p + big * (pi/2 - 2p)
  SDValue Fix =
      DAG.getNode(ISD::FSUB, DL, MVT::v4f32, getConst(M_PI_2, DL, DAG),
                  DAG.getNode(ISD::FADD, DL, MVT::v4f32, P, P));
  P = DAG.getNode(ISD::FADD, DL, MVT::v4f32, P,
                  DAG.getNode(ISD::FMUL, DL, MVT::v4f32, Fix, Big));
  // -p where x < 0: p - 2 p neg
  SDValue Neg = getPositiveMask(getNeg(X, DL, DAG), DL, DAG);
  SDValue P2 = DAG.getNode(ISD::FADD, DL, MVT::v4f32, P, P);
  return DAG.getNode(ISD::FSUB, DL, MVT::v4f32, P,
                     DAG.getNode(ISD::FMUL, DL, MVT::v4f32, P2, Neg));
}

static SDValue getSqrt(SDValue X, unsigned Field, const SDLoc &DL,
                       SelectionDAG &DAG) {
  SDValue Res = DAG.getUNDEF(MVT::v4f32);
  for (unsigned I = 0; I != 4; ++I) {
    if (Field != 4 && I != Field)
      continue;
    Res = DAG.getNode(
        ISD::INSERT_VECTOR_ELT, DL, MVT::v4f32, Res,
        DAG.getNode(ISD::FSQRT, DL, MVT::f32,
                    DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, MVT::f32, X,
                                DAG.getVectorIdxConstant(I, DL))),
        DAG.getVectorIdxConstant(I, DL));
  }
  return Res;
}

// Rounding and the math functions are computed on vectors, field by field:
// a scalar in the field it is in (a field of a vector stays in its place),
// with the field as the result.
SDValue VUTargetLowering::lowerMath(SDValue Op, SelectionDAG &DAG) const {
  SDLoc DL(Op);
  EVT VT = Op.getValueType();
  bool Scalar = VT == MVT::f32;
  unsigned Field = Scalar ? getScalarField(Op.getOperand(0)) : 4;
  auto Operand = [&](unsigned I) {
    SDValue V = Op.getOperand(I);
    return Scalar ? getScalarAsVector(V, Field, DL, DAG) : V;
  };
  SDValue V = Operand(0);
  auto Mul = [&](SDValue A, SDValue B) {
    return DAG.getNode(ISD::FMUL, DL, MVT::v4f32, A, B);
  };
  SDValue Res;
  switch (Op.getOpcode()) {
  case ISD::FEXP:
    Res = getExp2(Mul(V, getConst(M_LOG2E, DL, DAG)), DL, DAG);
    break;
  case ISD::FEXP2:
    Res = getExp2(V, DL, DAG);
    break;
  case ISD::FEXP10:
    Res = getExp2(Mul(V, getConst(3.321928094887362, DL, DAG)), DL, DAG);
    break;
  case ISD::FLOG:
    Res = Mul(getLog2(V, DL, DAG), getConst(M_LN2, DL, DAG));
    break;
  case ISD::FLOG2:
    Res = getLog2(V, DL, DAG);
    break;
  case ISD::FLOG10:
    Res = Mul(getLog2(V, DL, DAG), getConst(0.30102999566398120, DL, DAG));
    break;
  case ISD::FPOW:
    Res = getExp2(Mul(Operand(1), getLog2(V, DL, DAG)), DL, DAG);
    break;
  case ISD::FATAN:
    Res = getAtan(V, Field, DL, DAG);
    break;
  case ISD::FATAN2: {
    // atan(y / x), plus pi with the sign of y where x < 0. y / 0 is the
    // largest float with the sign of y: pi/2.
    SDValue X = Operand(1);
    Res = getAtan(getDivide(V, X, Field, DL, DAG), Field, DL, DAG);
    SDValue XNeg = getPositiveMask(getNeg(X, DL, DAG), DL, DAG);
    SDValue YNeg = getPositiveMask(getNeg(V, DL, DAG), DL, DAG);
    // pi * (1 - 2 * yneg) * xneg
    SDValue S = DAG.getNode(ISD::FSUB, DL, MVT::v4f32, getOnes(DL, DAG),
                            DAG.getNode(ISD::FADD, DL, MVT::v4f32, YNeg, YNeg));
    Res = DAG.getNode(ISD::FADD, DL, MVT::v4f32, Res,
                      Mul(Mul(S, XNeg), getConst(M_PI, DL, DAG)));
    break;
  }
  case ISD::FASIN:
  case ISD::FACOS: {
    // asin(x) = atan(x / sqrt(1 - x^2)), acos(x) = pi/2 - asin(x)
    SDValue D =
        DAG.getNode(ISD::FSUB, DL, MVT::v4f32, getOnes(DL, DAG), Mul(V, V));
    Res = getAtan(getDivide(V, getSqrt(D, Field, DL, DAG), Field, DL, DAG),
                  Field, DL, DAG);
    if (Op.getOpcode() == ISD::FACOS)
      Res = DAG.getNode(ISD::FSUB, DL, MVT::v4f32, getConst(M_PI_2, DL, DAG),
                        Res);
    break;
  }
  case ISD::FTAN:
    Res = getDivide(getSin(V, DL, DAG),
                    getSin(DAG.getNode(ISD::FADD, DL, MVT::v4f32, V,
                                       getConst(M_PI_2, DL, DAG)),
                           DL, DAG),
                    Field, DL, DAG);
    break;
  case ISD::FFLOOR:
    Res = getFloor(V, DL, DAG);
    break;
  case ISD::FCEIL:
    Res = getNeg(getFloor(getNeg(V, DL, DAG), DL, DAG), DL, DAG);
    break;
  case ISD::FROUND:
    // Halfway cases round up.
    Res =
        getFloor(DAG.getNode(ISD::FADD, DL, MVT::v4f32, V,
                             getBroadcast(DAG.getConstantFP(0.5, DL, MVT::f32),
                                          0, DL, DAG)),
                 DL, DAG);
    break;
  case ISD::FTRUNC:
    // FTOI0 rounds towards zero; |v| < 2^31.
    Res = DAG.getNode(ISD::SINT_TO_FP, DL, MVT::v4f32,
                      DAG.getNode(ISD::FP_TO_SINT, DL, MVT::v4i32, V));
    break;
  case ISD::FSIN:
    Res = getSin(V, DL, DAG);
    break;
  case ISD::FCOS:
    Res = getSin(DAG.getNode(ISD::FADD, DL, MVT::v4f32, V,
                             getBroadcast(DAG.getConstantFP(1.5707963267948966,
                                                            DL, MVT::f32),
                                          0, DL, DAG)),
                 DL, DAG);
    break;
  default:
    llvm_unreachable("unexpected math operation");
  }
  if (Scalar)
    return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, MVT::f32, Res,
                       DAG.getVectorIdxConstant(Field, DL));
  return Res;
}

SDValue VUTargetLowering::lowerBRCOND(SDValue Op, SelectionDAG &DAG) const {
  // A branch on 1 - m is one on m == 0.
  SDLoc DL(Op);
  SDValue C = Op.getOperand(1);
  if (C.getOpcode() != VUISD::MASK_INT)
    return Op;
  SDValue M = C.getOperand(0);
  if (M.getOpcode() != ISD::FSUB || !isOnes(M.getOperand(0)))
    return Op;
  SDValue Inv = DAG.getNode(VUISD::MASK_INT, DL, MVT::i16, M.getOperand(1),
                            C.getOperand(1));
  SDValue Cond = DAG.getSetCC(DL, MVT::i16, Inv,
                              DAG.getConstant(0, DL, MVT::i16), ISD::SETEQ);
  return DAG.getNode(ISD::BRCOND, DL, MVT::Other, Op.getOperand(0), Cond,
                     Op.getOperand(2));
}

SDValue VUTargetLowering::lowerXOR(SDValue Op, SelectionDAG &DAG) const {
  // There is no IXOR: a ^ b = (a | b) - (a & b).
  SDLoc DL(Op);
  SDValue A = Op.getOperand(0), B = Op.getOperand(1);
  return DAG.getNode(ISD::SUB, DL, MVT::i16,
                     DAG.getNode(ISD::OR, DL, MVT::i16, A, B),
                     DAG.getNode(ISD::AND, DL, MVT::i16, A, B));
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
  case ISD::XOR:
    return lowerXOR(Op, DAG);
  case ISD::BRCOND:
    return lowerBRCOND(Op, DAG);
  case ISD::FFLOOR:
  case ISD::FCEIL:
  case ISD::FTRUNC:
  case ISD::FROUND:
  case ISD::FSIN:
  case ISD::FCOS:
  case ISD::FTAN:
  case ISD::FEXP:
  case ISD::FEXP2:
  case ISD::FEXP10:
  case ISD::FLOG:
  case ISD::FLOG2:
  case ISD::FLOG10:
  case ISD::FPOW:
  case ISD::FATAN:
  case ISD::FATAN2:
  case ISD::FASIN:
  case ISD::FACOS:
    return lowerMath(Op, DAG);
  case ISD::SETCC:
    return lowerSETCC(Op, DAG);
  case ISD::SELECT:
  case ISD::VSELECT:
    return lowerSELECT(Op, DAG);
  case ISD::UINT_TO_FP:
    return lowerUINT_TO_FP(Op, DAG);
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

static SDValue getAsBroadcast(SDValue V, SelectionDAG &DAG);

static void addAccLeaf(SDValue V, bool Neg, SelectionDAG &DAG,
                       SmallVectorImpl<AccTerm> &Terms) {
  if (V.getOpcode() == ISD::FMUL && V.hasOneUse()) {
    SDValue X = V.getOperand(0), Y = V.getOperand(1), Src;
    // An operand that is not combined yet, such as max(v.x, 0), may be a
    // broadcast too.
    if (SDValue B = getAsBroadcast(Y, DAG))
      Y = B;
    else if (SDValue B = getAsBroadcast(X, DAG))
      X = B;
    unsigned Field;
    // With two broadcasts, the one from outside the block is the full
    // operand: its splat can be hoisted out of a loop, and the value made
    // here is read by its field.
    SDValue XSrc;
    unsigned XField;
    auto IsOutside = [](SDValue V) {
      return V.getOpcode() == ISD::CopyFromReg ||
             V.getOpcode() == ISD::Register || isConstOrConstSplatFP(V);
    };
    if (isBroadcast(Y, Src, Field) && isBroadcast(X, XSrc, XField) &&
        IsOutside(Src) && !IsOutside(XSrc))
      Terms.push_back({Y, XSrc, XField, Neg});
    else if (isBroadcast(Y, Src, Field))
      Terms.push_back({X, Src, Field, Neg});
    else if (isBroadcast(X, Src, Field))
      Terms.push_back({Y, Src, Field, Neg});
    else
      Terms.push_back({X, Y, VUISD::AccSumFull, Neg});
    return;
  }
  // A constant c = ones * c.x, which combineAccSum turns into an exact form
  // (the PS2's multiplier is 1 ULP low with 1.0 in fs). Other values
  // c = c * vf0.w: 1.0 in ft is exact.
  // (also a splat of it that is a broadcast already)
  ConstantFPSDNode *C = isConstOrConstSplatFP(V);
  if (!C && (V.getOpcode() == VUISD::SPLAT || V.getOpcode() == VUISD::BCAST))
    C = dyn_cast<ConstantFPSDNode>(V.getOperand(0));
  if (C) {
    SDLoc DL(V);
    SDValue Ones = DAG.getNode(VUISD::BCAST, DL, MVT::v4f32,
                               DAG.getRegister(VU::VF0, MVT::v4f32),
                               DAG.getTargetConstant(3, DL, MVT::i32));
    Terms.push_back(
        {Ones, DAG.getConstantFP(C->getValueAPF(), DL, MVT::f32), 0, Neg});
    return;
  }
  Terms.push_back({V, DAG.getRegister(VU::VF0, MVT::v4f32), 3, Neg});
}

/// Collects the terms of a left-linear chain of FADD and FSUB nodes, in
/// evaluation order.
static void collectAccTerms(SDValue V, SelectionDAG &DAG,
                            SmallVectorImpl<AccTerm> &Terms) {
  SumTerms S;
  collectSumTerms(
      V,
      [&](SDValue X) -> std::optional<bool> {
        if (X != V && !isAccChain(X))
          return std::nullopt;
        return X.getOpcode() == ISD::FSUB;
      },
      /*Flatten=*/false, S);
  for (const SumTerm &T : S.Terms)
    addAccLeaf(T.V, T.Neg, DAG, Terms);
}

/// Returns true if every FADD and FSUB of the chain may be reassociated.
static bool isReassocChain(SDValue V) {
  if (!V->getFlags().hasAllowReassociation())
    return false;
  for (SDValue Op : {V.getOperand(0), V.getOperand(1)})
    if (isAccChain(Op) && !isReassocChain(Op))
      return false;
  return true;
}

/// Returns true if \p V only depends on values from outside the block
/// (registers, constants, copies), such as a splat of a uniform, and not on
/// memory read here.
static bool isFromOutside(SDValue V, DenseMap<SDNode *, bool> &Known) {
  SDNode *N = V.getNode();
  if (N->getOpcode() == ISD::CopyFromReg)
    return true;
  if (auto It = Known.find(N); It != Known.end())
    return It->second;
  bool R = all_of(N->op_values(), [&](SDValue Op) {
    return Op.getValueType() != MVT::Other && Op.getValueType() != MVT::Glue &&
           isFromOutside(Op, Known);
  });
  return Known[N] = R;
}

// Computes a sum of products as MULA, MADDA..., MADD, which forwards the
// accumulator between the steps:
//   (m0 * v.x + m1 * v.y) + m2 * v.z + m3
//   -> MULAx acc, m0, v; MADDAy acc, m1, v; MADDAz acc, m2, v;
//      MADDw d, m3, vf0
// The terms are accumulated in the original order, so the result is the
// same.
static cl::opt<bool> AccChains("vu-acc-chains", cl::Hidden, cl::init(true),
                               cl::desc("Sums of products through ACC"));
static cl::opt<bool>
    AccOrder("vu-acc-order", cl::Hidden, cl::init(true),
             cl::desc("Reassociated ACC sums: outside terms first"));

SDValue VUTargetLowering::combineAccSum(SDNode *N, SelectionDAG &DAG) const {
  if (N->getValueType(0) != MVT::v4f32 || !AccChains)
    return SDValue();
  // Only the root of a chain.
  if (N->hasOneUse() && isAccChain(SDValue(*N->user_begin(), 0)))
    return SDValue();

  SmallVector<AccTerm, 8> Terms;
  collectAccTerms(SDValue(N, 0), DAG, Terms);
  // A constant term ones * c.x would have 1.0 in fs, which the PS2's
  // multiplier gets 1 ULP low: the last one is added after the products by a
  // broadcast ADD (the same instruction count, and the constant is still
  // read by its field), others are c * vf0.w with a splat of c.
  // (a product with a constant, a * c.x, is not one)
  auto IsConst = [](const AccTerm &T) {
    auto *R = T.A.getOpcode() == VUISD::BCAST
                  ? dyn_cast<RegisterSDNode>(T.A.getOperand(0))
                  : nullptr;
    return T.B.getOpcode() == ISD::ConstantFP && R && R->getReg() == VU::VF0 &&
           T.A.getConstantOperandVal(1) == 3;
  };
  SDLoc DL(N);
  // A last term that is a field of a register, b.y = splat(b.y) * vf0.w, is
  // added the same way, ADDy: no splat of the field, and a sum of one product
  // and a field, such as the fog p.w * f.x + f.y, does not take ACC from the
  // other sums.
  SDValue Last;
  bool LastNeg = false;
  SDValue LastSrc;
  unsigned LastField;
  const AccTerm &Back = Terms.back();
  auto *BackReg = dyn_cast<RegisterSDNode>(Back.B);
  if (IsConst(Back))
    Last = DAG.getNode(VUISD::BCAST, DL, MVT::v4f32, Back.B,
                       DAG.getTargetConstant(0, DL, MVT::i32));
  else if (BackReg && BackReg->getReg() == VU::VF0 && Back.Field == 3 &&
           isBroadcast(Back.A, LastSrc, LastField))
    Last = DAG.getNode(VUISD::BCAST, DL, MVT::v4f32, LastSrc,
                       DAG.getTargetConstant(LastField, DL, MVT::i32));
  if (Last) {
    LastNeg = Back.Neg;
    Terms.pop_back();
  }
  for (AccTerm &T : Terms)
    if (IsConst(T))
      T = {DAG.getNode(VUISD::SPLAT, DL, MVT::v4f32, T.B),
           DAG.getRegister(VU::VF0, MVT::v4f32), 3, T.Neg};
  // With reassociation, the terms of values from outside the block, such as
  // uniforms, go first, so a value computed here is only one MADD from the
  // result:
  //   amb + lcol * d.x  ->  MULAw acc, amb, vf0; MADDx r, lcol, d
  // (not MULAx acc, lcol, d; MADDw r, amb, vf0: one FMAC latency longer.)
  // Other terms keep their order: moving them only lengthens the time ACC is
  // held.
  if (AccOrder && isReassocChain(SDValue(N, 0))) {
    DenseMap<SDNode *, bool> Known;
    auto IsReady = [&](const AccTerm &T) {
      return isFromOutside(T.A, Known) && isFromOutside(T.B, Known);
    };
    SmallVector<AccTerm, 8> Sorted(Terms);
    std::stable_partition(Sorted.begin(), Sorted.end(), IsReady);
    if (!Sorted.empty() && !Sorted[0].Neg)
      Terms.assign(Sorted.begin(), Sorted.end());
  }
  // MULA can not negate; it is only worth it with two terms and a product.
  if (Terms.size() < 2 || Terms[0].Neg || none_of(Terms, [](const AccTerm &T) {
        return T.B.getOpcode() != ISD::Register;
      }))
    return SDValue();

  SmallVector<SDValue, 16> Ops;
  for (const AccTerm &T : Terms) {
    Ops.push_back(T.A);
    Ops.push_back(T.B);
    Ops.push_back(DAG.getTargetConstant(T.Field, DL, MVT::i32));
    Ops.push_back(DAG.getTargetConstant(T.Neg, DL, MVT::i32));
  }
  SDValue Sum = DAG.getNode(VUISD::ACCSUM, DL, MVT::v4f32, Ops);
  if (!Last)
    return Sum;
  return DAG.getNode(LastNeg ? ISD::FSUB : ISD::FADD, DL, MVT::v4f32, Sum, Last,
                     N->getFlags());
}

/// Returns true if \p V is field 1-3 of a vector, which a scalar operation
/// would have to move to the x field first.
static bool isUpperLane(SDValue V) {
  return V.getOpcode() == ISD::EXTRACT_VECTOR_ELT &&
         isa<ConstantSDNode>(V.getOperand(1)) &&
         V.getConstantOperandVal(1) != 0;
}

static bool isScalarSum(SDValue V) {
  return (V.getOpcode() == ISD::FADD || V.getOpcode() == ISD::FSUB) &&
         V.getValueType() == MVT::f32 && V.hasOneUse();
}

/// Collects the terms of a left-linear chain of scalar FADD and FSUB nodes:
/// a field k of a vector is vf0 * v.k, which is v.k in the w field.
static void collectLaneTerms(SDValue V, SelectionDAG &DAG,
                             SmallVectorImpl<AccTerm> &Terms) {
  SDValue L = V.getOperand(0), R = V.getOperand(1);
  if (V.getOpcode() == ISD::FADD && !isScalarSum(L) && isScalarSum(R))
    std::swap(L, R);
  SDValue VF0 = DAG.getRegister(VU::VF0, MVT::v4f32);
  auto addLeaf = [&](SDValue Leaf, bool Neg) {
    if (Leaf.getOpcode() == ISD::EXTRACT_VECTOR_ELT &&
        isa<ConstantSDNode>(Leaf.getOperand(1)))
      Terms.push_back({VF0, Leaf.getOperand(0),
                       unsigned(Leaf.getConstantOperandVal(1)), Neg});
    else
      Terms.push_back({VF0, Leaf, 0, Neg});
  };
  if (isScalarSum(L))
    collectLaneTerms(L, DAG, Terms);
  else
    addLeaf(L, /*Neg=*/false);
  addLeaf(R, V.getOpcode() == ISD::FSUB);
}

// Adds fields of vectors, like a dot product, in the field of the first
// term, the others added as broadcasts:
//   (t.y + t.z) + t.x  ->  ADDz.y d, t, t.z; ADDx.y d, d, t.x
// The scalar result is d.y; selection reads it from there, so no field is
// moved to x. Each step rounds like the scalar sum. (Through ACC the others
// would be ones * t.k, with 1.0 in fs: 1 ULP low on the PS2.)
static SDValue combineLaneSum(SDNode *N, SelectionDAG &DAG) {
  if (N->getValueType(0) != MVT::f32 ||
      (N->hasOneUse() && isScalarSum(SDValue(*N->user_begin(), 0))))
    return SDValue();
  SmallVector<AccTerm, 4> Terms;
  collectLaneTerms(SDValue(N, 0), DAG, Terms);
  if (Terms[0].Neg || Terms.size() < 2 || Terms.size() > 4 ||
      none_of(Terms, [](const AccTerm &T) { return T.Field != 0; }))
    return SDValue();
  SDLoc DL(N);
  unsigned Field = Terms[0].Field;
  SDValue Sum = Terms[0].B;
  if (Sum.getValueType() != MVT::v4f32)
    Sum = DAG.getNode(ISD::SCALAR_TO_VECTOR, DL, MVT::v4f32, Sum);
  for (const AccTerm &T : drop_begin(Terms))
    Sum = DAG.getNode(T.Neg ? ISD::FSUB : ISD::FADD, DL, MVT::v4f32, Sum,
                      DAG.getNode(VUISD::BCAST, DL, MVT::v4f32, T.B,
                                  DAG.getTargetConstant(T.Field, DL, MVT::i32)),
                      N->getFlags());
  return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, MVT::f32, Sum,
                     DAG.getVectorIdxConstant(Field, DL));
}

// A scalar operation on field k of a vector is done on the whole vector,
// with the other operand broadcast, instead of moving the field to x first:
//   max(v.w, 0)  ->  MAXx.w d, v, vf0x
static SDValue combineLaneOp(SDNode *N, SelectionDAG &DAG) {
  if (N->getValueType(0) != MVT::f32)
    return SDValue();
  SDValue Lane = N->getOperand(0), Other = N->getOperand(1);
  if (!isUpperLane(Lane) && N->getOpcode() != ISD::FSUB)
    std::swap(Lane, Other);
  if (!isUpperLane(Lane))
    return SDValue();
  SDLoc DL(N);
  SDValue Splat;
  if (isUpperLane(Other))
    Splat = DAG.getNode(
        VUISD::BCAST, DL, MVT::v4f32, Other.getOperand(0),
        DAG.getTargetConstant(Other.getConstantOperandVal(1), DL, MVT::i32));
  else if (isNullFPConstant(Other))
    Splat = DAG.getNode(VUISD::BCAST, DL, MVT::v4f32,
                        DAG.getRegister(VU::VF0, MVT::v4f32),
                        DAG.getTargetConstant(0, DL, MVT::i32));
  else
    Splat = DAG.getNode(VUISD::BCAST, DL, MVT::v4f32, Other,
                        DAG.getTargetConstant(0, DL, MVT::i32));
  SDValue Op = DAG.getNode(N->getOpcode(), DL, MVT::v4f32, Lane.getOperand(0),
                           Splat, N->getFlags());
  return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, MVT::f32, Op,
                     Lane.getOperand(1));
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

/// \p V divided by its factor \p C, a power of two, if it has one as an
/// operand of a product that may be reassociated, or of a scalar that is
/// splat: (a * C) * b -> a * b, splat (s * C) -> splat s.
static SDValue stripFactor(SDValue V, double C, SelectionDAG &DAG,
                           unsigned Depth = 0) {
  if (Depth > 3 || !V.hasOneUse())
    return SDValue();
  SDLoc DL(V);
  EVT VT = V.getValueType();
  if (V.getOpcode() == ISD::FMUL && V->getFlags().hasAllowReassociation()) {
    SDValue A = V.getOperand(0), B = V.getOperand(1);
    for (int I = 0; I != 2; ++I, std::swap(A, B)) {
      if (ConstantFPSDNode *CN = isConstOrConstSplatFP(B);
          CN && CN->isExactlyValue(C))
        return A;
      if (SDValue A2 = stripFactor(A, C, DAG, Depth + 1))
        return DAG.getNode(ISD::FMUL, DL, VT, A2, B, V->getFlags());
    }
    return SDValue();
  }
  // A splat of a scalar with the factor.
  if (VT.isVector() && V.getOpcode() != ISD::FMUL) {
    if (V.getOpcode() == VUISD::SPLAT || V.getOpcode() == VUISD::BCAST) {
      if (V.getOperand(0).getValueType() != MVT::f32)
        return SDValue();
      if (SDValue S = stripFactor(V.getOperand(0), C, DAG, Depth + 1)) {
        SmallVector<SDValue, 2> Ops(V->ops());
        Ops[0] = S;
        return DAG.getNode(V.getOpcode(), DL, VT, Ops);
      }
      return SDValue();
    }
    if (SDValue S = DAG.getSplatValue(V))
      if (SDValue S2 = stripFactor(S, C, DAG, Depth + 1))
        return DAG.getSplat(VT, DL, S2);
  }
  return SDValue();
}

static bool isBroadcastPairOp(unsigned Opc) {
  return Opc == ISD::FADD || Opc == ISD::FSUB || Opc == ISD::FMUL ||
         Opc == ISD::FMAXNUM || Opc == ISD::FMINNUM;
}

static SDValue combineBroadcastPair(SDNode *N, SelectionDAG &DAG);

/// \p V as a broadcast of one field: V itself, or an operation on two
/// broadcasts made into one (combineBroadcastPair). The combiner may visit
/// an operation before its operands.
static SDValue getAsBroadcast(SDValue V, SelectionDAG &DAG) {
  if (V.getOpcode() == VUISD::BCAST &&
      V.getOperand(0).getValueType() == MVT::v4f32)
    return V;
  if (isBroadcastPairOp(V.getOpcode()) && V.hasOneUse())
    return combineBroadcastPair(V.getNode(), DAG);
  return SDValue();
}

// An operation on two broadcasts, or on a broadcast and a constant splat, is
// one on a field, broadcast:
//   op (bcast a, i), (bcast b, j)  ->  bcast (op a, (bcast b, j)), i
// The operation reads b.j by its broadcast field, and the result keeps the
// value in field i, which later instructions read by their broadcast field
// too, instead of making the two splats (two instructions each).
static SDValue combineBroadcastPair(SDNode *N, SelectionDAG &DAG) {
  if (N->getValueType(0) != MVT::v4f32 || !isBroadcastPairOp(N->getOpcode()))
    return SDValue();
  SDValue A = N->getOperand(0), B = N->getOperand(1);
  bool Commutes = N->getOpcode() != ISD::FSUB;
  auto IsSplat = [](SDValue V) { return isConstOrConstSplatFP(V) != nullptr; };
  SDValue BA = getAsBroadcast(A, DAG);
  if (!BA && Commutes && IsSplat(A)) {
    std::swap(A, B);
    BA = getAsBroadcast(A, DAG);
  }
  if (!BA)
    return SDValue();
  // The other operand is read by its broadcast field: any broadcast does,
  // also one of a scalar.
  SDValue BB =
      IsSplat(B) || B.getOpcode() == VUISD::BCAST ? B : getAsBroadcast(B, DAG);
  if (!BB)
    return SDValue();
  SDLoc DL(N);
  SDValue Op = DAG.getNode(N->getOpcode(), DL, MVT::v4f32, BA.getOperand(0), BB,
                           N->getFlags());
  return DAG.getNode(VUISD::BCAST, DL, MVT::v4f32, Op, BA.getOperand(1));
}

namespace {
/// A field of a vector.
struct VUField {
  SDValue Vec;
  unsigned F;
};
} // namespace

/// The field that lane \p Lane of \p V is: a field extracted from a vector,
/// a field of a shuffle, or the field of the vector itself.
static std::optional<VUField> getLaneField(SDValue V, unsigned Lane) {
  if (V.getOpcode() == ISD::EXTRACT_VECTOR_ELT &&
      V.getOperand(0).getValueType() == MVT::v4f32)
    if (auto *C = dyn_cast<ConstantSDNode>(V.getOperand(1)))
      if (C->getZExtValue() < 4)
        return VUField{V.getOperand(0), (unsigned)C->getZExtValue()};
  if (V.getValueType() != MVT::v4f32)
    return std::nullopt;
  if (auto *SV = dyn_cast<ShuffleVectorSDNode>(V)) {
    int M = SV->getMaskElt(Lane);
    if (M < 0)
      return std::nullopt;
    return VUField{V.getOperand(M / 4), (unsigned)M % 4};
  }
  return VUField{V, Lane};
}

/// Whether lane \p Lane of \p D is field \p I of the cross product A x B,
/// A[j] * B[k] - A[k] * B[j] with j = I + 1 and k = I + 2 (mod 3). A and B
/// are set by the first lane that is matched.
static bool matchCrossLane(SDValue D, unsigned Lane, unsigned I, SDValue &A,
                           SDValue &B) {
  if (D.getOpcode() != ISD::FSUB || D.getOperand(0).getOpcode() != ISD::FMUL ||
      D.getOperand(1).getOpcode() != ISD::FMUL)
    return false;
  auto Factors = [&](SDValue M) -> std::optional<std::pair<VUField, VUField>> {
    auto X = getLaneField(M.getOperand(0), Lane);
    auto Y = getLaneField(M.getOperand(1), Lane);
    if (!X || !Y)
      return std::nullopt;
    return std::make_pair(*X, *Y);
  };
  auto P = Factors(D.getOperand(0)), Q = Factors(D.getOperand(1));
  if (!P || !Q)
    return false;
  unsigned J = (I + 1) % 3, K = (I + 2) % 3;
  // Product A[F1] * B[F2], in either order of its factors.
  auto Is = [&](std::pair<VUField, VUField> Prod, unsigned F1, unsigned F2,
                SDValue &PA, SDValue &PB) {
    for (bool Swap : {false, true}) {
      VUField X = Swap ? Prod.second : Prod.first;
      VUField Y = Swap ? Prod.first : Prod.second;
      if (X.F == F1 && Y.F == F2 && (!PA || (PA == X.Vec && PB == Y.Vec))) {
        PA = X.Vec;
        PB = Y.Vec;
        return true;
      }
    }
    return false;
  };
  SDValue NA = A, NB = B;
  if (!Is(*P, J, K, NA, NB) || !Is(*Q, K, J, NA, NB))
    return false;
  A = NA;
  B = NB;
  return true;
}

/// The cross product A x B of fields x, y and z: OPMULA.xyz ACC, A, B and
/// OPMSUB.xyz d, B, A. Field w of the result is undefined.
static SDValue getCross(SDValue A, SDValue B, const SDLoc &DL,
                        SelectionDAG &DAG) {
  return DAG.getNode(VUISD::CROSS, DL, MVT::v4f32, A, B);
}

SDValue VUTargetLowering::PerformDAGCombine(SDNode *N,
                                            DAGCombinerInfo &DCI) const {
  SelectionDAG &DAG = DCI.DAG;
  SDLoc DL(N);
  switch (N->getOpcode()) {
  case ISD::SINT_TO_FP:
  case ISD::UINT_TO_FP: {
    // A comparison result as a float is its mask (true is -1 for a signed
    // i1). This is done before the types are legalized: v4i1 would become
    // v4i32, and there are no i32 constants.
    SDValue V = N->getOperand(0);
    if (V.getOpcode() != VUISD::MASK_INT &&
        V.getValueType().getScalarType() != MVT::i1)
      break;
    auto [M, Field] = getMask(V, DAG);
    if (!M)
      break;
    if (N->getOpcode() == ISD::SINT_TO_FP && V.getOpcode() != VUISD::MASK_INT)
      M = DAG.getNode(ISD::FSUB, DL, MVT::v4f32,
                      DAG.getConstantFP(0.0, DL, MVT::v4f32), M);
    if (N->getValueType(0) == MVT::v4f32)
      return M;
    return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, MVT::f32, M,
                       DAG.getVectorIdxConstant(Field, DL));
  }
  case ISD::FDIV: {
    // x / splat (y) with arcp: x * splat (1 / y), one DIV instead of four.
    // Before the vector division is split into fields.
    if (N->getValueType(0) != MVT::v4f32 || !N->getFlags().hasAllowReciprocal())
      break;
    SDValue Y = DAG.getSplatValue(N->getOperand(1));
    if (!Y)
      break;
    SDValue R =
        DAG.getNode(ISD::FDIV, DL, MVT::f32,
                    DAG.getConstantFP(1.0, DL, MVT::f32), Y, N->getFlags());
    return DAG.getNode(ISD::FMUL, DL, MVT::v4f32, N->getOperand(0),
                       DAG.getSplat(MVT::v4f32, DL, R), N->getFlags());
  }
  case ISD::SELECT:
  case ISD::VSELECT: {
    // Before the types are legalized, for the same reason.
    EVT VT = N->getValueType(0);
    if ((VT == MVT::f32 || VT == MVT::v4f32) &&
        N->getOperand(0).getValueType().getScalarType() == MVT::i1 &&
        getMask(N->getOperand(0), DAG).first)
      return lowerSELECT(SDValue(N, 0), DAG);
    break;
  }
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
    // The factor deeper in the product, after reassociation:
    // fp_to_sint (p * splat (q * 16)) -> FTOI4 (p * splat q)
    for (int Bits : {4, 12, 15})
      if (SDValue X = stripFactor(Mul, std::ldexp(1.0, Bits), DAG))
        return DAG.getNode(VUISD::FTOI, DL, MVT::v4i32, X,
                           DAG.getTargetConstant(Bits, DL, MVT::i32));
    break;
  }
  case ISD::FMUL: {
    if (DCI.isAfterLegalizeDAG() && N->getValueType(0) == MVT::f32)
      return combineLaneOp(N, DAG);
    if (SDValue R = combineBroadcastPair(N, DAG))
      return R;
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
      // sint_to_fp (v) * (b * 2^-n) -> ITOF<n> (v) * b, after reassociation
      if (N->getFlags().hasAllowReassociation())
        for (int Bits : {4, 12, 15})
          if (SDValue B = stripFactor(N->getOperand(1 - I),
                                      std::ldexp(1.0, -Bits), DAG))
            return DAG.getNode(
                ISD::FMUL, DL, MVT::v4f32,
                DAG.getNode(VUISD::ITOF, DL, MVT::v4f32, Conv.getOperand(0),
                            DAG.getTargetConstant(Bits, DL, MVT::i32)),
                B, N->getFlags());
    }
    break;
  }
  case ISD::BUILD_VECTOR: {
    // A cross product computed field by field (HLSL cross).
    if (N->getValueType(0) == MVT::v4f32 && !DCI.isAfterLegalizeDAG()) {
      SDValue A, B;
      if (matchCrossLane(N->getOperand(0), 0, 0, A, B) &&
          matchCrossLane(N->getOperand(1), 1, 1, A, B) &&
          matchCrossLane(N->getOperand(2), 2, 2, A, B)) {
        SDValue Cross = getCross(A, B, DL, DAG);
        if (N->getOperand(3).isUndef())
          return Cross;
        return DAG.getNode(ISD::INSERT_VECTOR_ELT, DL, MVT::v4f32, Cross,
                           N->getOperand(3), DAG.getVectorIdxConstant(3, DL));
      }
    }
    // An integer vector, before i32 is legalized (there are no i32
    // registers): constants are the bits of float constants, and 16-bit
    // integers are inserted with MFIR.
    if (N->getValueType(0) != MVT::v4i32 || DCI.isAfterLegalizeDAG())
      break;
    bool AnyNarrow = false;
    SmallVector<SDValue, 4> Fs(4);
    SDValue First;
    for (unsigned I = 0; I != 4; ++I) {
      SDValue E = N->getOperand(I);
      if (E.isUndef())
        continue;
      if (auto *C = dyn_cast<ConstantSDNode>(E)) {
        Fs[I] = DAG.getConstantFP(
            APFloat(APFloat::IEEEsingle(), C->getAPIntValue().trunc(32)), DL,
            MVT::f32);
        if (!First)
          First = Fs[I];
        continue;
      }
      if ((E.getOpcode() != ISD::SIGN_EXTEND &&
           E.getOpcode() != ISD::ANY_EXTEND) ||
          E.getOperand(0).getValueType() != MVT::i16)
        return SDValue();
      AnyNarrow = true;
    }
    // The constant fields: not a BUILD_VECTOR of floats, whose bitcast would
    // be folded back into this one (before the types are legalized), which
    // would never end; a SPLAT of a constant and the others inserted, as
    // lowerBUILD_VECTOR makes them.
    SDValue V = DAG.getUNDEF(MVT::v4i32);
    if (First) {
      SDValue F = DAG.getNode(VUISD::SPLAT, DL, MVT::v4f32, First);
      for (unsigned I = 0; I != 4; ++I)
        if (Fs[I] && Fs[I] != First)
          F = DAG.getNode(ISD::INSERT_VECTOR_ELT, DL, MVT::v4f32, F, Fs[I],
                          DAG.getVectorIdxConstant(I, DL));
      V = DAG.getNode(ISD::BITCAST, DL, MVT::v4i32, F);
    }
    if (!AnyNarrow)
      return V;
    for (unsigned I = 0; I != 4; ++I)
      if (SDValue E = N->getOperand(I); !E.isUndef() && !Fs[I])
        V = DAG.getNode(VUISD::INSERT_VI, DL, MVT::v4i32, V, E.getOperand(0),
                        DAG.getTargetConstant(I, DL, MVT::i32));
    return V;
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
  case ISD::EXTRACT_VECTOR_ELT: {
    // A field of a broadcast is the broadcast field.
    SDValue Src = N->getOperand(0);
    if (Src.getOpcode() != VUISD::BCAST ||
        !isa<ConstantSDNode>(N->getOperand(1)))
      break;
    SDValue V = Src.getOperand(0);
    if (V.getValueType() == MVT::f32)
      return V;
    return DAG.getNode(
        ISD::EXTRACT_VECTOR_ELT, DL, MVT::f32, V,
        DAG.getVectorIdxConstant(Src.getConstantOperandVal(1), DL));
  }
  case VUISD::BCAST: {
    // The x field of a field of a vector is that field of the vector.
    SDValue Src = N->getOperand(0);
    if (N->getConstantOperandVal(1) != 0 || !isUpperLane(Src))
      break;
    return DAG.getNode(
        VUISD::BCAST, DL, MVT::v4f32, Src.getOperand(0),
        DAG.getTargetConstant(Src.getConstantOperandVal(1), DL, MVT::i32));
  }
  case VUISD::ACCSUM: {
    // A term that broadcasts the x field of a field of a vector broadcasts
    // that field of the vector.
    SmallVector<SDValue, 16> Ops(N->ops());
    bool Changed = false;
    for (unsigned I = 0, E = Ops.size(); I != E; I += 4)
      if (Ops[I + 2]->getAsZExtVal() == 0 && isUpperLane(Ops[I + 1])) {
        Ops[I + 2] = DAG.getTargetConstant(Ops[I + 1].getConstantOperandVal(1),
                                           DL, MVT::i32);
        Ops[I + 1] = Ops[I + 1].getOperand(0);
        Changed = true;
      }
    if (!Changed)
      break;
    return DAG.getNode(VUISD::ACCSUM, DL, MVT::v4f32, Ops);
  }
  case VUISD::SPLAT: {
    // 1.0 is vf0.w.
    if (auto *C = dyn_cast<ConstantFPSDNode>(N->getOperand(0)))
      if (C->isExactlyValue(1.0))
        return DAG.getNode(VUISD::BCAST, DL, MVT::v4f32,
                           DAG.getRegister(VU::VF0, MVT::v4f32),
                           DAG.getTargetConstant(3, DL, MVT::i32));
    // Broadcast straight from the field: no move to the x field first.
    SDValue Src;
    unsigned Field;
    isBroadcast(SDValue(N, 0), Src, Field);
    return DAG.getNode(VUISD::BCAST, DL, MVT::v4f32, Src,
                       DAG.getTargetConstant(Field, DL, MVT::i32));
  }
  case ISD::FADD:
  case ISD::FSUB:
    // A cross product of vectors whose field w is not needed: undefined in
    // the shuffles, or replaced by the only user.
    if (N->getOpcode() == ISD::FSUB && N->getValueType(0) == MVT::v4f32 &&
        !DCI.isAfterLegalizeDAG()) {
      SDValue A, B, D(N, 0);
      bool WUnused = (N->hasOneUse() &&
                      N->user_begin()->getOpcode() == ISD::INSERT_VECTOR_ELT &&
                      N->user_begin()->getOperand(0) == D &&
                      isa<ConstantSDNode>(N->user_begin()->getOperand(2)) &&
                      N->user_begin()->getConstantOperandVal(2) == 3) ||
                     all_of(N->op_values(), [](SDValue M) {
                       return M.getOpcode() == ISD::FMUL &&
                              all_of(M->op_values(), [](SDValue S) {
                                auto *SV = dyn_cast<ShuffleVectorSDNode>(S);
                                return SV && SV->getMaskElt(3) < 0;
                              });
                     });
      if (WUnused && matchCrossLane(D, 0, 0, A, B) &&
          matchCrossLane(D, 1, 1, A, B) && matchCrossLane(D, 2, 2, A, B))
        return getCross(A, B, DL, DAG);
    }
    if (SDValue R = combineBroadcastPair(N, DAG))
      return R;
    if (!DCI.isAfterLegalizeDAG())
      break;
    if (N->getValueType(0) == MVT::f32) {
      if (SDValue Sum = combineLaneSum(N, DAG))
        return Sum;
      return combineLaneOp(N, DAG);
    }
    return combineAccSum(N, DAG);
  case ISD::FMAXNUM:
  case ISD::FMINNUM:
    if (DCI.isAfterLegalizeDAG() && N->getValueType(0) == MVT::f32)
      return combineLaneOp(N, DAG);
    return combineBroadcastPair(N, DAG);
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
