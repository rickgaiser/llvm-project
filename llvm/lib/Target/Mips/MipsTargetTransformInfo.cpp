//===-- MipsTargetTransformInfo.cpp - Mips specific TTI ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MipsTargetTransformInfo.h"
#include "MipsSubtarget.h"
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

  // VU0 native v4f32 operations are cheap
  if (ST->hasVU0() && Ty->isVectorTy()) {
    auto *VTy = cast<VectorType>(Ty);
    if (VTy->getElementType()->isFloatTy() &&
        VTy->getElementCount() == ElementCount::getFixed(4)) {
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
  // VU0 v4f32 load/store via LQC2/SQC2
  if (ST->hasVU0() && Src->isVectorTy()) {
    auto *VTy = cast<VectorType>(Src);
    if (VTy->getElementType()->isFloatTy() &&
        VTy->getElementCount() == ElementCount::getFixed(4)) {
      // LQC2/SQC2 require 16-byte alignment
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
  // VU0 v4f32 extract/insert costs
  if (ST->hasVU0() && Val->isVectorTy()) {
    auto *VTy = cast<VectorType>(Val);
    if (VTy->getElementType()->isFloatTy() &&
        VTy->getElementCount() == ElementCount::getFixed(4)) {
      if (Opcode == Instruction::ExtractElement) {
        // QMFC2 + shift + truncate + MTC1
        // Constant index is slightly cheaper
        return (Index == -1U) ? 5 : 3;
      }
      if (Opcode == Instruction::InsertElement) {
        // QMFC2 + shift + insert + QMTC2
        return (Index == -1U) ? 7 : 5;
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
  // VU0 v4f32 shuffle costs
  VectorType *Tp = SrcTy ? SrcTy : DstTy;
  if (ST->hasVU0() && Tp && Tp->getElementType()->isFloatTy() &&
      Tp->getElementCount() == ElementCount::getFixed(4)) {
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
