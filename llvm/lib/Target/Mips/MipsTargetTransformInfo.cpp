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

  // R5900 MMI native integer vector operations
  if (ST->isR5900() && Ty->isVectorTy()) {
    auto *VTy = cast<VectorType>(Ty);
    unsigned BitWidth = VTy->getPrimitiveSizeInBits();

    // Only 128-bit vectors are supported
    if (BitWidth == 128) {
      Type *EltTy = VTy->getElementType();
      if (EltTy->isIntegerTy()) {
        unsigned EltBits = EltTy->getIntegerBitWidth();

        switch (Opcode) {
        case Instruction::Add:
        case Instruction::Sub:
          // PADDW/PSUBW (32-bit), PADDH/PSUBH (16-bit), PADDB/PSUBB (8-bit)
          if (EltBits == 8 || EltBits == 16 || EltBits == 32)
            return 1; // Native MMI operation
          break;

        case Instruction::And:
        case Instruction::Or:
        case Instruction::Xor:
          // PAND, POR, PXOR - all element sizes use same instruction
          return 1; // Native MMI operation

        default:
          break;
        }
      }
    }
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

  // R5900 MMI 128-bit integer vector load/store via LQ/SQ
  if (ST->isR5900() && Src->isVectorTy()) {
    auto *VTy = cast<VectorType>(Src);
    if (VTy->getPrimitiveSizeInBits() == 128) {
      if (Alignment >= Align(16))
        return 1; // Single aligned 128-bit load/store
      return 8;   // Unaligned requires scalar fallback
    }
  }

  return BaseT::getMemoryOpCost(Opcode, Src, Alignment, AddressSpace, CostKind,
                                OpInfo, I);
}

InstructionCost MipsTTIImpl::getVectorInstrCost(unsigned Opcode, Type *Val,
                                                TTI::TargetCostKind CostKind,
                                                unsigned Index, const Value *Op0,
                                                const Value *Op1) const {
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

  // R5900 MMI integer vector extract/insert costs
  if (ST->isR5900() && Val->isVectorTy()) {
    auto *VTy = cast<VectorType>(Val);
    if (VTy->getPrimitiveSizeInBits() == 128 &&
        VTy->getElementType()->isIntegerTy()) {
      if (Opcode == Instruction::ExtractElement)
        return 2; // Shift + truncate
      if (Opcode == Instruction::InsertElement)
        return 4; // More complex manipulation
    }
  }

  return BaseT::getVectorInstrCost(Opcode, Val, CostKind, Index, Op0, Op1);
}

InstructionCost MipsTTIImpl::getShuffleCost(TTI::ShuffleKind Kind,
                                            VectorType *DstTy, VectorType *SrcTy,
                                            ArrayRef<int> Mask,
                                            TTI::TargetCostKind CostKind,
                                            int Index, VectorType *SubTp,
                                            ArrayRef<const Value *> Args,
                                            const Instruction *CxtI) const {
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

  // R5900 MMI integer vector shuffle costs
  if (ST->isR5900() && Tp->getPrimitiveSizeInBits() == 128 &&
      Tp->getElementType()->isIntegerTy()) {
    switch (Kind) {
    case TTI::SK_Broadcast:
      // Use PCPYH/PCPYLD for replication
      return 2;
    case TTI::SK_Select:
      return 2; // AND/OR masking
    case TTI::SK_PermuteSingleSrc:
      return 4;
    case TTI::SK_PermuteTwoSrc:
      return 8;
    default:
      break;
    }
  }

  return BaseT::getShuffleCost(Kind, DstTy, SrcTy, Mask, CostKind, Index, SubTp,
                               Args, CxtI);
}

InstructionCost MipsTTIImpl::getIntrinsicInstrCost(
    const IntrinsicCostAttributes &ICA, TTI::TargetCostKind CostKind) const {
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
        // FMA lowering: FMUL + FADD (no direct fused multiply-add with single latency)
        // VMADD requires ACC setup, so it's effectively 2 operations
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

  // R5900 MMI integer vector intrinsic costs
  if (ST->isR5900() && RetTy->isVectorTy()) {
    auto *VTy = dyn_cast<FixedVectorType>(RetTy);
    if (VTy && VTy->getPrimitiveSizeInBits() == 128 &&
        VTy->getElementType()->isIntegerTy()) {
      switch (ICA.getID()) {
      case Intrinsic::abs:
        // PABSW (v4i32), PABSH (v8i16)
        if (VTy->getElementType()->getIntegerBitWidth() >= 16)
          return 1;
        break;
      case Intrinsic::smin:
      case Intrinsic::smax:
        // PMINW/PMAXW (v4i32), PMINH/PMAXH (v8i16)
        if (VTy->getElementType()->getIntegerBitWidth() >= 16)
          return 1;
        break;
      case Intrinsic::sadd_sat:
      case Intrinsic::ssub_sat:
      case Intrinsic::uadd_sat:
      case Intrinsic::usub_sat:
        // PADDSW/PSUBSW/PADDUW/PSUBUW etc
        return 1;
      case Intrinsic::ctlz:
        // PLZCW counts leading zeros in each 32-bit word
        if (VTy->getElementType()->getIntegerBitWidth() == 32)
          return 1;
        break;
      default:
        break;
      }
    }
  }

  return BaseT::getIntrinsicInstrCost(ICA, CostKind);
}

//===----------------------------------------------------------------------===//
// Reduction Costs
//===----------------------------------------------------------------------===//
// R5900 MMI and VU0 have no horizontal reduction instructions, so reductions
// are done via tree reduction patterns (shuffle + op at each level).

InstructionCost MipsTTIImpl::getArithmeticReductionCost(
    unsigned Opcode, VectorType *Ty, std::optional<FastMathFlags> FMF,
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
  // Total: ~24 cycles, which is much worse than scalar FPU ACC chain (~4 cycles)
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

  // MMI v4i32 reduction (tree reduction)
  if (ST->isR5900() && EltTy->isIntegerTy(32) && NumElts == 4) {
    switch (Opcode) {
    case Instruction::Add:
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
      // 2 shuffle + 2 ops + final extract
      return 5;
    default:
      break;
    }
  }

  // MMI v8i16 reduction
  if (ST->isR5900() && EltTy->isIntegerTy(16) && NumElts == 8) {
    switch (Opcode) {
    case Instruction::Add:
      // 3 levels for 8 elements + extracts
      return 8;
    default:
      break;
    }
  }

  return BaseT::getArithmeticReductionCost(Opcode, Ty, FMF, CostKind);
}

InstructionCost MipsTTIImpl::getMinMaxReductionCost(Intrinsic::ID IID,
                                                    VectorType *Ty,
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

  // MMI v4i32 min/max reduction (PMAXW/PMINW + tree reduction)
  if (ST->isR5900() && EltTy->isIntegerTy(32) && NumElts == 4) {
    if (IID == Intrinsic::smax || IID == Intrinsic::smin) {
      // 2 shuffle + 2 PMAXW/PMINW + final extract
      return 5;
    }
  }

  // MMI v8i16 min/max reduction (PMAXH/PMINH + tree reduction)
  if (ST->isR5900() && EltTy->isIntegerTy(16) && NumElts == 8) {
    if (IID == Intrinsic::smax || IID == Intrinsic::smin) {
      // 3 shuffle + 3 PMAXH/PMINH + final extract
      return 8;
    }
  }

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
    // Truncation within 128-bit is relatively cheap (PPAC* instructions)
    if (DstBits <= 128 && SrcBits <= 128)
      return 2;
    break;

  case Instruction::ZExt:
  case Instruction::SExt:
    // Extension within 128-bit uses PEXTL* instructions
    if (DstBits <= 128 && SrcBits <= 128) {
      // v8i16 -> v4i32: PEXTLH or similar
      // v16i8 -> v8i16: PEXTLB or similar
      return 2;
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

  // ICmp for MMI vectors
  if (Opcode == Instruction::ICmp && ST->isR5900()) {
    if (EltTy->isIntegerTy(32) && NumElts == 4) {
      // PCGTW/PCEQW: single instruction
      return 1;
    }
    if (EltTy->isIntegerTy(16) && NumElts == 8) {
      // PCGTH/PCEQH: single instruction
      return 1;
    }
    if (EltTy->isIntegerTy(8) && NumElts == 16) {
      // PCGTB/PCEQB: single instruction
      return 1;
    }
  }

  // FCmp for VU0 - no native vector compare, expensive
  if (Opcode == Instruction::FCmp && ST->hasVU0()) {
    if (EltTy->isFloatTy() && NumElts == 4) {
      // No native v4f32 compare - scalarizes
      return 10;
    }
  }

  // Select operations
  if (Opcode == Instruction::Select) {
    // MMI select via PAND/POR masking
    if (ST->isR5900() && VTy->getPrimitiveSizeInBits() == 128) {
      // mask AND val1, NOT mask AND val2, OR results = 3 ops
      return 3;
    }
    // VU0 has no native masked select - expensive
    if (ST->hasVU0() && EltTy->isFloatTy() && NumElts == 4) {
      return 8;
    }
  }

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

  auto *VTy = dyn_cast<FixedVectorType>(VecTy);
  if (!VTy)
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
