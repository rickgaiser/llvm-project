//===-- VUTargetTransformInfo.h - VU specific TTI ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The VU has 31 VF registers of four floats and 15 VI registers of 16 bits.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_VU_VUTARGETTRANSFORMINFO_H
#define LLVM_LIB_TARGET_VU_VUTARGETTRANSFORMINFO_H

#include "VUTargetMachine.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/BasicTTIImpl.h"

namespace llvm {
class VUTTIImpl final : public BasicTTIImplBase<VUTTIImpl> {
  typedef BasicTTIImplBase<VUTTIImpl> BaseT;
  typedef TargetTransformInfo TTI;
  friend BaseT;

  const VUSubtarget *ST;
  const VUTargetLowering *TLI;

  const VUSubtarget *getST() const { return ST; }
  const VUTargetLowering *getTLI() const { return TLI; }

public:
  explicit VUTTIImpl(const VUTargetMachine *TM, const Function &F)
      : BaseT(TM, F.getDataLayout()), ST(TM->getSubtargetImpl(F)),
        TLI(ST->getTargetLowering()) {}

  unsigned getNumberOfRegisters(unsigned ClassID) const override {
    // Class 1 is vectors.
    return ClassID == 1 ? 31 : 15;
  }
  TypeSize
  getRegisterBitWidth(TargetTransformInfo::RegisterKind K) const override {
    switch (K) {
    case TargetTransformInfo::RGK_Scalar:
      return TypeSize::getFixed(16);
    case TargetTransformInfo::RGK_FixedWidthVector:
      return TypeSize::getFixed(128);
    case TargetTransformInfo::RGK_ScalableVector:
      return TypeSize::getScalable(0);
    }
    llvm_unreachable("unknown register kind");
  }
  unsigned getMinVectorRegisterBitWidth() const override { return 128; }

  // A broadcast is free as the broadcast field of an FMAC operand, and the
  // fields of a vector that stay in their places are free too; every other
  // field is moved by an instruction of its own (INSERT_FIELD).
  InstructionCost
  getShuffleCost(TTI::ShuffleKind Kind, VectorType *DstTy, VectorType *SrcTy,
                 TTI::TargetCostKind CostKind, ArrayRef<int> Mask, int Index,
                 VectorType *SubTp, ArrayRef<const Value *> Args = {},
                 const Instruction *CtxI = nullptr,
                 TTI::VectorInstrContext VIC =
                     TTI::VectorInstrContext::None) const override {
    auto *DT = dyn_cast<FixedVectorType>(DstTy);
    if (!DT)
      return BaseT::getShuffleCost(Kind, DstTy, SrcTy, CostKind, Mask, Index,
                                   SubTp, Args, CtxI, VIC);
    unsigned N = DT->getNumElements();
    switch (Kind) {
    case TTI::SK_Broadcast:
      return 0;
    case TTI::SK_ExtractSubvector:
      return Index == 0 ? 0 : N;
    case TTI::SK_InsertSubvector:
      return SubTp ? cast<FixedVectorType>(SubTp)->getNumElements() : N;
    default:
      break;
    }
    if (Mask.empty())
      return N;
    unsigned SrcN = cast<FixedVectorType>(SrcTy)->getNumElements();
    unsigned Moved = 0;
    bool SecondInPlace = false;
    for (auto [I, M] : enumerate(Mask)) {
      if (M < 0 || M == int(I))
        continue;
      // Fields of the second vector in their places: one masked move.
      if (M == int(I + SrcN)) {
        SecondInPlace = true;
        continue;
      }
      ++Moved;
    }
    return Moved + SecondInPlace;
  }

  using BaseT::getVectorInstrCost;
  InstructionCost
  getVectorInstrCost(unsigned Opcode, Type *Val, TTI::TargetCostKind CostKind,
                     unsigned Index, const Value *Op0, const Value *Op1,
                     TTI::VectorInstrContext VIC =
                         TTI::VectorInstrContext::None) const override {
    // Field x is the scalar; another field is read by its broadcast field
    // or moved to x.
    if (Opcode == Instruction::ExtractElement)
      return Index == 0 ? 0 : 1;
    return 1;
  }
  unsigned getMaxInterleaveFactor(ElementCount VF,
                                  bool HasReductions) const override {
    return 1;
  }
};
} // namespace llvm

#endif // LLVM_LIB_TARGET_VU_VUTARGETTRANSFORMINFO_H
