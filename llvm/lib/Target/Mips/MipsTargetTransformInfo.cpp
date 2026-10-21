//===-- MipsTargetTransformInfo.cpp - Mips specific TTI ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MipsTargetTransformInfo.h"
#include "MipsSubtarget.h"
#include "llvm/Analysis/IVDescriptors.h"
#include "llvm/IR/DerivedTypes.h"

using namespace llvm;

bool MipsTTIImpl::hasDivRemOp(Type *DataType, bool IsSigned) const {
  EVT VT = TLI->getValueType(DL, DataType);
  return TLI->isOperationLegalOrCustom(IsSigned ? ISD::SDIVREM : ISD::UDIVREM,
                                       VT);
}

bool MipsTTIImpl::isLSRCostLess(const TargetTransformInfo::LSRCost &C1,
                                const TargetTransformInfo::LSRCost &C2) const {
  // MIPS specific here are "instruction number 1st priority".
  // If we need to emit adds inside the loop to add up base registers, then
  // we need at least one extra temporary register.
  unsigned C1NumRegs = C1.NumRegs + (C1.NumBaseAdds != 0);
  unsigned C2NumRegs = C2.NumRegs + (C2.NumBaseAdds != 0);
  return std::tie(C1.Insns, C1NumRegs, C1.AddRecCost, C1.NumIVMuls,
                  C1.NumBaseAdds, C1.ScaleCost, C1.ImmCost, C1.SetupCost) <
         std::tie(C2.Insns, C2NumRegs, C2.AddRecCost, C2.NumIVMuls,
                  C2.NumBaseAdds, C2.ScaleCost, C2.ImmCost, C2.SetupCost);
}

void MipsTTIImpl::getUnrollingPreferences(
    Loop *L, ScalarEvolution &SE, TTI::UnrollingPreferences &UP,
    OptimizationRemarkEmitter *ORE) const {
  BaseT::getUnrollingPreferences(L, SE, UP, ORE);
  if (!ST->isR5900() || UP.Partial)
    return;

  // Partial and runtime unrolling gives the R5900 more independent
  // instructions to issue together (measured).
  for (BasicBlock *BB : L->blocks())
    for (Instruction &I : *BB)
      if (const auto *CB = dyn_cast<CallBase>(&I))
        if (!CB->getCalledFunction() ||
            isLoweredToCall(CB->getCalledFunction()))
          return;

  UP.Partial = UP.Runtime = UP.UpperBound = true;
  UP.PartialThreshold = 120;
  UP.OptSizeThreshold = 0;
  UP.PartialOptSizeThreshold = 0;
  UP.BEInsns = 2;
}

//===----------------------------------------------------------------------===//
// R5900 Vectorization Support (VU0 and MMI)
//===----------------------------------------------------------------------===//

unsigned MipsTTIImpl::getNumberOfRegisters(unsigned ClassID) const {
  bool Vector = (ClassID == 1);
  if (Vector) {
    if (ST->hasVU0())
      return 32; // VF0-VF31 (though VF0 is a constant register)
    if (ST->isR5900())
      return 30; // GPR128 vector registers (excluding $zero, $at)
  }
  return 32; // GP registers
}

TypeSize
MipsTTIImpl::getRegisterBitWidth(TargetTransformInfo::RegisterKind K) const {
  switch (K) {
  case TargetTransformInfo::RGK_Scalar:
    return TypeSize::getFixed(ST->isGP64bit() ? 64 : 32);
  case TargetTransformInfo::RGK_FixedWidthVector:
    if (ST->hasVU0() || ST->isR5900())
      return TypeSize::getFixed(128); // VU0 VF regs or R5900 GPR128
    return TypeSize::getFixed(0);
  case TargetTransformInfo::RGK_ScalableVector:
    return TypeSize::getScalable(0); // No scalable vector support
  }
  llvm_unreachable("Unsupported register kind");
}

unsigned MipsTTIImpl::getMinVectorRegisterBitWidth() const {
  if (ST->hasVU0() || ST->isR5900())
    return 128;
  return 0;
}

// R5900 MMI costs are in cycles, from the bench uarch tests on hardware:
//   - an MMI operation (paddw, pcgtw, pmaxw, pand, psllw and the pextlw,
//     ppacw, pcpyld, pcpyud, pexew, pcpyh, prot3w shuffles) has a latency of 1
//     and issues one per cycle. It uses both integer pipes, so it does not pair
//     with an ALU operation, but it does pair with a load or store.
//   - lq and sq issue one per cycle, like the other loads and stores; lq has a
//     latency of 2.
//   - a load of a just stored value does not stall (sq->lw: 3 cycles).
// Operations without a single MMI instruction are lowered through a stack
// temporary, so their cost is the number of loads and stores.

std::optional<std::pair<InstructionCost, MVT>>
MipsTTIImpl::getMMIType(Type *Ty) const {
  auto *VTy = dyn_cast<FixedVectorType>(Ty);
  if (!ST->isR5900() || !VTy || !VTy->getElementType()->isIntegerTy() ||
      VTy->getPrimitiveSizeInBits() % 128 != 0)
    return std::nullopt;
  std::pair<InstructionCost, MVT> LT = getTypeLegalizationCost(VTy);
  if (!LT.second.isVector() || LT.second.getSizeInBits() != 128)
    return std::nullopt;
  return LT;
}

InstructionCost
MipsTTIImpl::getMMIScalarizedCost(FixedVectorType *VTy, unsigned NumVecOps,
                                  InstructionCost ScalarCost,
                                  TTI::TargetCostKind CostKind) const {
  return VTy->getNumElements() * ScalarCost +
         getScalarizationOverhead(VTy, /*Insert=*/true, /*Extract=*/false,
                                  CostKind) +
         NumVecOps * getScalarizationOverhead(VTy, /*Insert=*/false,
                                              /*Extract=*/true, CostKind);
}

InstructionCost MipsTTIImpl::getScalarizationOverhead(
    VectorType *Ty, const APInt &DemandedElts, bool Insert, bool Extract,
    TTI::TargetCostKind CostKind, bool ForPoisonSrc, ArrayRef<Value *> VL,
    TTI::VectorInstrContext VIC) const {
  auto LT = getMMIType(Ty);
  if (!LT)
    return BaseT::getScalarizationOverhead(Ty, DemandedElts, Insert, Extract,
                                           CostKind, ForPoisonSrc, VL, VIC);

  // Per 128-bit part: extracting is an sq and a load per element, inserting a
  // store per element and an lq (plus an sq of the old value if only some
  // elements are replaced).
  unsigned PartElts = LT->second.getVectorNumElements();
  unsigned NumElts = cast<FixedVectorType>(Ty)->getNumElements();
  InstructionCost Cost = 0;
  for (unsigned Part = 0; Part < NumElts; Part += PartElts) {
    unsigned Demanded = 0;
    for (unsigned I = Part; I < Part + PartElts; ++I)
      Demanded += DemandedElts[I];
    if (!Demanded)
      continue;
    if (Extract)
      Cost += 1 + Demanded;
    if (Insert)
      Cost += Demanded + 1 + (!ForPoisonSrc && Demanded != PartElts);
  }
  return Cost;
}

InstructionCost MipsTTIImpl::getArithmeticInstrCost(
    unsigned Opcode, Type *Ty, TTI::TargetCostKind CostKind,
    TTI::OperandValueInfo Op1Info, TTI::OperandValueInfo Op2Info,
    ArrayRef<const Value *> Args, const Instruction *CxtI) const {

  // VU0 vector float operations
  if (ST->hasVU0() && Ty->isVectorTy()) {
    auto *VTy = cast<VectorType>(Ty);
    if (VTy->getElementType()->isFloatTy()) {
      unsigned NumElts = VTy->getElementCount().getKnownMinValue();

      // Only v4f32 is natively supported on VU0.
      // Other widths (v2f32, v8f32, etc.) require expensive scalarization
      // or widening/splitting which adds significant overhead.
      if (NumElts != 4) {
        // Very expensive: scalarization or widening costs
        // v2f32: widens to v4f32, wastes half the vector width
        // Plus insertelement/extractelement overhead
        return 20;
      }

      // Native v4f32 operations
      switch (Opcode) {
      case Instruction::FAdd:
      case Instruction::FSub:
      case Instruction::FMul:
        return 1; // Native VU0 operation (4 cycle latency, 1 throughput)
      case Instruction::FDiv:
        // VDIV is sequential per component: Q = fs[fsf] / ft[ftf]
        // Must execute 4 times with VWAITQ between each
        return 29; // 4 * (7 cycle latency) + overhead
      case Instruction::FRem:
        // No native FRem support - very expensive scalarization
        return 100;
      default:
        break;
      }
    }
  }

  // R5900 MMI integer vector operations
  if (auto LT = getMMIType(Ty)) {
    unsigned EltBits = LT->second.getScalarSizeInBits();
    switch (Opcode) {
    case Instruction::Add:
    case Instruction::Sub:
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
      // paddb/h/w, psubb/h/w, pand, por, pxor. The 64-bit element forms are
      // currently scalarized.
      if (EltBits != 64)
        return LT->first;
      break;
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:
      // psllh/w, psrlh/w, psrah/w only take an immediate shift amount.
      if ((EltBits == 16 || EltBits == 32) && Op2Info.isUniform() &&
          Op2Info.isConstant())
        return LT->first;
      break;
    default:
      break;
    }
    // Everything else, including multiply, is scalarized.
    auto *VTy = cast<FixedVectorType>(Ty);
    InstructionCost ScalarCost =
        getArithmeticInstrCost(Opcode, VTy->getElementType(), CostKind,
                               Op1Info.getNoProps(), Op2Info.getNoProps());
    unsigned NumVecOps = Op2Info.isConstant() ? 1 : 2;
    return getMMIScalarizedCost(VTy, NumVecOps, ScalarCost, CostKind);
  }

  return BaseT::getArithmeticInstrCost(Opcode, Ty, CostKind, Op1Info, Op2Info,
                                       Args, CxtI);
}

InstructionCost MipsTTIImpl::getMemoryOpCost(unsigned Opcode, Type *Src,
                                             Align Alignment,
                                             unsigned AddressSpace,
                                             TTI::TargetCostKind CostKind,
                                             TTI::OperandValueInfo OpInfo,
                                             const Instruction *I) const {
  // VU0 vector float load/store via LQC2/SQC2
  if (ST->hasVU0() && Src->isVectorTy()) {
    auto *VTy = cast<VectorType>(Src);
    if (VTy->getElementType()->isFloatTy()) {
      unsigned NumElts = VTy->getElementCount().getKnownMinValue();

      // Only v4f32 is natively supported for load/store
      if (NumElts != 4) {
        // Non-native widths require scalarization or widening
        return 15;
      }

      // Native v4f32 LQC2/SQC2 require 16-byte alignment
      if (Alignment >= Align(16))
        return 1; // Single aligned 128-bit load/store
      // Unaligned access requires scalar fallback
      return 8;
    }
  }

  // R5900 MMI integer vector load/store, per 128-bit part: an aligned lq/sq
  // takes 1 cycle. An unaligned one goes through a stack temporary: ldl/ldr x2,
  // sd x2, lq takes 10 cycles and sq, ld x2, sdl/sdr x2 takes 6.
  if (auto LT = getMMIType(Src)) {
    if (Alignment >= Align(16))
      return LT->first;
    return LT->first * (Opcode == Instruction::Load ? 10 : 6);
  }

  return BaseT::getMemoryOpCost(Opcode, Src, Alignment, AddressSpace, CostKind,
                                OpInfo, I);
}

InstructionCost MipsTTIImpl::getVectorInstrCost(
    unsigned Opcode, Type *Val, TTI::TargetCostKind CostKind, unsigned Index,
    const Value *Op0, const Value *Op1, TTI::VectorInstrContext VIC) const {
  // VU0 vector float extract/insert costs
  // These are expensive because VU0 has no direct element access - must go
  // through GPR128:
  //   Extract: QMFC2 + DSRL32 (for upper elements) + MTC1 = 4-6 cycles
  //   Insert: QMFC2 + GPR manipulation + QMTC2 = 6-10 cycles
  // Variable index is even more expensive due to computed shifts.
  if (ST->hasVU0() && Val->isVectorTy()) {
    auto *VTy = cast<VectorType>(Val);
    if (VTy->getElementType()->isFloatTy()) {
      unsigned NumElts = VTy->getElementCount().getKnownMinValue();

      // Non-native vector widths are very expensive
      if (NumElts != 4) {
        // Widening/scalarization overhead plus element access
        return (Opcode == Instruction::ExtractElement) ? 10 : 15;
      }

      // Native v4f32 costs
      if (Opcode == Instruction::ExtractElement) {
        // QMFC2 (2 cy) + DSRL32 (1 cy) + MTC1 (2 cy) + possible shift/mask
        return (Index == -1U) ? 8 : 5;
      }
      if (Opcode == Instruction::InsertElement) {
        // QMFC2 (2 cy) + GPR mask/shift (2-4 cy) + QMTC2 (2 cy + 1 cy stall)
        return (Index == -1U) ? 12 : 8;
      }
    }
  }

  // R5900 MMI integer vector extract/insert, through a stack temporary.
  if (getMMIType(Val)) {
    // sq, then a load of the element.
    if (Opcode == Instruction::ExtractElement)
      return 2;
    if (Opcode == Instruction::InsertElement) {
      // Into an undefined vector: a store of the element and an lq.
      if (Op0 && isa<UndefValue>(Op0))
        return 2;
      // Otherwise the element is merged in memory: 12 loads and stores.
      return 12;
    }
  }

  return BaseT::getVectorInstrCost(Opcode, Val, CostKind, Index, Op0, Op1, VIC);
}

InstructionCost MipsTTIImpl::getShuffleCost(
    TTI::ShuffleKind Kind, VectorType *DstTy, VectorType *SrcTy,
    TTI::TargetCostKind CostKind, ArrayRef<int> Mask, int Index,
    VectorType *SubTp, ArrayRef<const Value *> Args, const Instruction *CxtI,
    TTI::VectorInstrContext VIC) const {
  // VU0 vector float shuffle costs
  VectorType *Tp = SrcTy ? SrcTy : DstTy;
  if (ST->hasVU0() && Tp && Tp->getElementType()->isFloatTy()) {
    unsigned NumElts = Tp->getElementCount().getKnownMinValue();

    // Non-native vector widths are very expensive
    if (NumElts != 4) {
      // Widening/scalarization overhead plus shuffle
      return 15;
    }

    // Native v4f32 shuffle costs
    switch (Kind) {
    case TTI::SK_Broadcast:
      // Splat: need to replicate one element to all positions
      // QMFC2 + shift + replicate (PCPYH/PCPYLD) + QMTC2
      return 4;
    case TTI::SK_Select:
      // Blend/select between two vectors using VU0 masked move
      return 1;
    case TTI::SK_PermuteSingleSrc:
      // Permute within single vector: QMFC2 + GPR shuffle + QMTC2
      return 6;
    case TTI::SK_PermuteTwoSrc:
      // Permute from two vectors: more complex
      return 12;
    case TTI::SK_Reverse:
      // Reverse order: GPR manipulation
      return 6;
    case TTI::SK_Splice:
      // Concatenate and extract: expensive
      return 10;
    default:
      break;
    }
  }

  // R5900 MMI integer vector shuffles are lowered through a stack temporary:
  // per 128-bit result part, an sq per source, a load and a store per element
  // and an lq. A broadcast loads the element once.
  if (auto LT = getMMIType(DstTy)) {
    unsigned PartElts = LT->second.getVectorNumElements();
    if (Kind == TTI::SK_Broadcast)
      return LT->first * (PartElts + 1) + 2;
    unsigned NumSrcs =
        Kind == TTI::SK_PermuteTwoSrc || Kind == TTI::SK_Select ||
                Kind == TTI::SK_Transpose || Kind == TTI::SK_Splice
            ? 2
            : 1;
    return LT->first * (NumSrcs + 2 * PartElts + 1);
  }

  return BaseT::getShuffleCost(Kind, DstTy, SrcTy, CostKind, Mask, Index, SubTp,
                               Args, CxtI, VIC);
}

InstructionCost
MipsTTIImpl::getIntrinsicInstrCost(const IntrinsicCostAttributes &ICA,
                                   TTI::TargetCostKind CostKind) const {
  Type *RetTy = ICA.getReturnType();

  // VU0 v4f32 intrinsic costs
  if (ST->hasVU0() && RetTy->isVectorTy()) {
    auto *VTy = dyn_cast<FixedVectorType>(RetTy);
    if (VTy && VTy->getElementType()->isFloatTy() &&
        VTy->getNumElements() == 4) {
      switch (ICA.getID()) {
      case Intrinsic::fabs:
        return 1; // VABS
      case Intrinsic::minnum:
      case Intrinsic::maxnum:
        return 1; // VMINI/VMAX
      case Intrinsic::fma:
      case Intrinsic::fmuladd:
        // FMA lowering: FMUL + FADD (no direct fused multiply-add with single
        // latency) VMADD requires ACC setup, so it's effectively 2 operations
        return 2;
      case Intrinsic::sqrt:
        // VSQRT is sequential per component (Q = sqrt(ft[ftf]))
        // Must execute 4 times with VWAITQ between each
        return 32; // 4 * 8 cycle latency
      case Intrinsic::copysign:
        // Need to extract sign bits, combine - fairly expensive
        return 8;
      default:
        break;
      }
    }
  }

  // R5900 MMI integer vector intrinsics with a single MMI instruction
  if (auto LT = getMMIType(RetTy)) {
    unsigned EltBits = LT->second.getScalarSizeInBits();
    switch (ICA.getID()) {
    case Intrinsic::abs:
    case Intrinsic::smin:
    case Intrinsic::smax:
      // pabsh/w, pminh/w, pmaxh/w
      if (EltBits == 16 || EltBits == 32)
        return LT->first;
      break;
    case Intrinsic::sadd_sat:
    case Intrinsic::ssub_sat:
    case Intrinsic::uadd_sat:
    case Intrinsic::usub_sat:
      // paddsb/h/w, psubsb/h/w, paddub/h/w, psubub/h/w
      if (EltBits != 64)
        return LT->first;
      break;
    default:
      break;
    }
  }

  return BaseT::getIntrinsicInstrCost(ICA, CostKind);
}

//===----------------------------------------------------------------------===//
// Reduction Costs
//===----------------------------------------------------------------------===//
// R5900 MMI and VU0 have no horizontal reduction instructions, so reductions
// are done via tree reduction patterns (shuffle + op at each level).

InstructionCost
MipsTTIImpl::getArithmeticReductionCost(unsigned Opcode, VectorType *Ty,
                                        std::optional<FastMathFlags> FMF,
                                        TTI::TargetCostKind CostKind) const {

  // Only handle R5900/VU0 vectors
  if (!ST->hasVU0() && !ST->isR5900())
    return BaseT::getArithmeticReductionCost(Opcode, Ty, FMF, CostKind);

  auto *VTy = dyn_cast<FixedVectorType>(Ty);
  if (!VTy)
    return BaseT::getArithmeticReductionCost(Opcode, Ty, FMF, CostKind);

  unsigned NumElts = VTy->getNumElements();
  Type *EltTy = VTy->getElementType();

  // VU0 v4f32 reduction (no horizontal ops, tree reduction)
  // This is VERY expensive on VU0 because there are no native horizontal
  // reduction instructions. The tree reduction requires:
  //   Level 1: QMFC2 + GPR shuffle + QMTC2 + VADD = ~10 cycles
  //   Level 2: QMFC2 + GPR shuffle + QMTC2 + VADD = ~10 cycles
  //   Extract: QMFC2 + MTC1 = ~4 cycles
  // Total: ~24 cycles, which is much worse than scalar FPU ACC chain (~4
  // cycles)
  if (ST->hasVU0() && EltTy->isFloatTy() && NumElts == 4) {
    switch (Opcode) {
    case Instruction::FAdd:
    case Instruction::FMul:
      // High cost to discourage vectorization of reduction patterns
      return 24;
    default:
      break;
    }
  }

  // MMI integer reductions use the generic shuffle tree costs.
  return BaseT::getArithmeticReductionCost(Opcode, Ty, FMF, CostKind);
}

InstructionCost
MipsTTIImpl::getMinMaxReductionCost(Intrinsic::ID IID, VectorType *Ty,
                                    FastMathFlags FMF,
                                    TTI::TargetCostKind CostKind) const {
  // Only handle R5900/VU0 vectors
  if (!ST->hasVU0() && !ST->isR5900())
    return BaseT::getMinMaxReductionCost(IID, Ty, FMF, CostKind);

  auto *VTy = dyn_cast<FixedVectorType>(Ty);
  if (!VTy)
    return BaseT::getMinMaxReductionCost(IID, Ty, FMF, CostKind);

  unsigned NumElts = VTy->getNumElements();
  Type *EltTy = VTy->getElementType();

  // VU0 v4f32 min/max reduction (VMAX/VMINI + tree reduction)
  // Same expensive shuffle path as arithmetic reductions
  if (ST->hasVU0() && EltTy->isFloatTy() && NumElts == 4) {
    // Level 1: QMFC2 + GPR shuffle + QMTC2 + VMAX/VMINI = ~10 cycles
    // Level 2: QMFC2 + GPR shuffle + QMTC2 + VMAX/VMINI = ~10 cycles
    // Extract: QMFC2 + MTC1 = ~4 cycles
    return 24;
  }

  // MMI integer reductions use the generic shuffle tree costs.
  return BaseT::getMinMaxReductionCost(IID, Ty, FMF, CostKind);
}

//===----------------------------------------------------------------------===//
// Cast Instruction Costs
//===----------------------------------------------------------------------===//

InstructionCost MipsTTIImpl::getCastInstrCost(unsigned Opcode, Type *Dst,
                                              Type *Src,
                                              TTI::CastContextHint CCH,
                                              TTI::TargetCostKind CostKind,
                                              const Instruction *I) const {
  // Only handle R5900/VU0 vectors
  if (!ST->hasVU0() && !ST->isR5900())
    return BaseT::getCastInstrCost(Opcode, Dst, Src, CCH, CostKind, I);

  auto *DstVTy = dyn_cast<FixedVectorType>(Dst);
  auto *SrcVTy = dyn_cast<FixedVectorType>(Src);

  // Only handle vector casts
  if (!DstVTy || !SrcVTy)
    return BaseT::getCastInstrCost(Opcode, Dst, Src, CCH, CostKind, I);

  unsigned DstBits = DstVTy->getPrimitiveSizeInBits();
  unsigned SrcBits = SrcVTy->getPrimitiveSizeInBits();

  switch (Opcode) {
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
    // MMI integer vector extensions and truncations are scalarized.
    if (getMMIType(Src) || getMMIType(Dst)) {
      InstructionCost ScalarCost =
          getCastInstrCost(Opcode, DstVTy->getElementType(),
                           SrcVTy->getElementType(), CCH, CostKind);
      return getScalarizationOverhead(SrcVTy, /*Insert=*/false,
                                      /*Extract=*/true, CostKind) +
             DstVTy->getNumElements() * ScalarCost +
             getScalarizationOverhead(DstVTy, /*Insert=*/true,
                                      /*Extract=*/false, CostKind);
    }
    break;

  case Instruction::FPToSI:
  case Instruction::FPToUI:
  case Instruction::SIToFP:
  case Instruction::UIToFP:
    // VU0 lacks native vector FP<->Int conversion (VFTOI/VITOF not implemented)
    // These scalarize, which is very expensive
    if (ST->hasVU0() && SrcVTy->getElementType()->isFloatTy() &&
        SrcVTy->getNumElements() == 4) {
      // 4 extracts + 4 scalar converts + 4 inserts
      return 20;
    }
    break;

  case Instruction::BitCast:
    // Bitcast between same-size 128-bit vectors is free
    if (DstBits == 128 && SrcBits == 128)
      return 0;
    break;

  default:
    break;
  }

  return BaseT::getCastInstrCost(Opcode, Dst, Src, CCH, CostKind, I);
}

//===----------------------------------------------------------------------===//
// Compare and Select Costs
//===----------------------------------------------------------------------===//

InstructionCost MipsTTIImpl::getCmpSelInstrCost(
    unsigned Opcode, Type *ValTy, Type *CondTy, CmpInst::Predicate VecPred,
    TTI::TargetCostKind CostKind, TTI::OperandValueInfo Op1Info,
    TTI::OperandValueInfo Op2Info, const Instruction *I) const {

  // Only handle R5900/VU0 vectors
  if (!ST->hasVU0() && !ST->isR5900())
    return BaseT::getCmpSelInstrCost(Opcode, ValTy, CondTy, VecPred, CostKind,
                                     Op1Info, Op2Info, I);

  auto *VTy = dyn_cast<FixedVectorType>(ValTy);
  if (!VTy)
    return BaseT::getCmpSelInstrCost(Opcode, ValTy, CondTy, VecPred, CostKind,
                                     Op1Info, Op2Info, I);

  unsigned NumElts = VTy->getNumElements();
  Type *EltTy = VTy->getElementType();

  // MMI integer vector compares and selects are scalarized.
  if ((Opcode == Instruction::ICmp || Opcode == Instruction::Select) &&
      getMMIType(VTy)) {
    InstructionCost ScalarCost = getCmpSelInstrCost(
        Opcode, EltTy, CondTy ? CondTy->getScalarType() : nullptr, VecPred,
        CostKind, Op1Info.getNoProps(), Op2Info.getNoProps());
    return getMMIScalarizedCost(VTy, 2, ScalarCost, CostKind);
  }

  // FCmp for VU0 - no native vector compare, expensive
  if (Opcode == Instruction::FCmp && ST->hasVU0()) {
    if (EltTy->isFloatTy() && NumElts == 4) {
      // No native v4f32 compare - scalarizes
      return 10;
    }
  }

  // VU0 has no native masked select - expensive
  if (Opcode == Instruction::Select && ST->hasVU0() && EltTy->isFloatTy() &&
      NumElts == 4)
    return 8;

  return BaseT::getCmpSelInstrCost(Opcode, ValTy, CondTy, VecPred, CostKind,
                                   Op1Info, Op2Info, I);
}

//===----------------------------------------------------------------------===//
// Reduction Preferences
//===----------------------------------------------------------------------===//

bool MipsTTIImpl::preferInLoopReduction(RecurKind Kind, Type *Ty) const {
  // For VU0/R5900 with floating-point reductions, prefer in-loop (scalar)
  // reductions. The FPU accumulator chain (MULA.S/MADDA.S/MADD.S) is far more
  // efficient than vectorizing and then doing expensive horizontal reductions
  // which require QMFC2 + GPR shuffles + element extraction.
  //
  // VU0 has no native horizontal reduction instructions, so a v4f32 fadd
  // reduction requires: shuffle + fadd + shuffle + fadd + extract = ~20 cycles
  // vs the scalar ACC chain which pipelines efficiently at ~4 cycles.
  if (ST->hasVU0() && Ty->isFloatTy()) {
    switch (Kind) {
    case RecurKind::FAdd:
    case RecurKind::FMul:
    case RecurKind::FMulAdd:
      return true;
    default:
      break;
    }
  }

  return false;
}

//===----------------------------------------------------------------------===//
// Interleaved Memory Access Costs
//===----------------------------------------------------------------------===//

InstructionCost MipsTTIImpl::getInterleavedMemoryOpCost(
    unsigned Opcode, Type *VecTy, unsigned Factor, ArrayRef<unsigned> Indices,
    Align Alignment, unsigned AddressSpace, TTI::TargetCostKind CostKind,
    bool UseMaskForCond, bool UseMaskForGaps) const {

  // Only handle R5900/VU0 vectors
  if (!ST->hasVU0() && !ST->isR5900())
    return BaseT::getInterleavedMemoryOpCost(Opcode, VecTy, Factor, Indices,
                                             Alignment, AddressSpace, CostKind,
                                             UseMaskForCond, UseMaskForGaps);

  // MMI integer vectors: the generic cost from the load/store and shuffle
  // costs, as the (de)interleaving shuffles go through a stack temporary.
  auto *VTy = dyn_cast<FixedVectorType>(VecTy);
  if (!VTy || getMMIType(VTy))
    return BaseT::getInterleavedMemoryOpCost(Opcode, VecTy, Factor, Indices,
                                             Alignment, AddressSpace, CostKind,
                                             UseMaskForCond, UseMaskForGaps);

  // Unaligned interleaved access is very expensive
  if (Alignment < Align(16)) {
    return InstructionCost::getInvalid();
  }

  // Basic cost: Factor * (load/store cost) + shuffle overhead
  InstructionCost MemCost = Factor; // Each LQ/SQ is 1 cycle

  // Shuffle overhead depends on factor and element arrangement
  InstructionCost ShuffleCost;
  switch (Factor) {
  case 2:
    // Interleave factor 2: relatively simple shuffle
    ShuffleCost = 2;
    break;
  case 3:
    // Factor 3: more complex shuffling
    ShuffleCost = 4;
    break;
  case 4:
    // Factor 4: significant shuffle overhead
    ShuffleCost = 6;
    break;
  default:
    // Higher factors: fall back to base cost
    return BaseT::getInterleavedMemoryOpCost(Opcode, VecTy, Factor, Indices,
                                             Alignment, AddressSpace, CostKind,
                                             UseMaskForCond, UseMaskForGaps);
  }

  return MemCost + ShuffleCost;
}
