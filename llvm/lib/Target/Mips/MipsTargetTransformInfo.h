//===-- MipsTargetTransformInfo.h - Mips specific TTI -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MIPS_MIPSTARGETTRANSFORMINFO_H
#define LLVM_LIB_TARGET_MIPS_MIPSTARGETTRANSFORMINFO_H

#include "MipsTargetMachine.h"
#include "llvm/Analysis/IVDescriptors.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/BasicTTIImpl.h"
#include "llvm/IR/Instructions.h"

namespace llvm {

class MipsTTIImpl final : public BasicTTIImplBase<MipsTTIImpl> {
  using BaseT = BasicTTIImplBase<MipsTTIImpl>;
  using TTI = TargetTransformInfo;

  friend BaseT;

  const MipsSubtarget *ST;
  const MipsTargetLowering *TLI;

  const MipsSubtarget *getST() const { return ST; }
  const MipsTargetLowering *getTLI() const { return TLI; }

public:
  explicit MipsTTIImpl(const MipsTargetMachine *TM, const Function &F)
      : BaseT(TM, F.getDataLayout()), ST(TM->getSubtargetImpl(F)),
        TLI(ST->getTargetLowering()) {}

  bool hasDivRemOp(Type *DataType, bool IsSigned) const override;

  bool isLSRCostLess(const TargetTransformInfo::LSRCost &C1,
                     const TargetTransformInfo::LSRCost &C2) const override;

  /// \name Vector TTI Implementations
  /// @{

  unsigned getNumberOfRegisters(unsigned ClassID) const override;

  TypeSize
  getRegisterBitWidth(TargetTransformInfo::RegisterKind K) const override;

  unsigned getMinVectorRegisterBitWidth() const override;

  InstructionCost getArithmeticInstrCost(
      unsigned Opcode, Type *Ty, TTI::TargetCostKind CostKind,
      TTI::OperandValueInfo Op1Info = {TTI::OK_AnyValue, TTI::OP_None},
      TTI::OperandValueInfo Op2Info = {TTI::OK_AnyValue, TTI::OP_None},
      ArrayRef<const Value *> Args = {},
      const Instruction *CxtI = nullptr) const override;

  InstructionCost getMemoryOpCost(unsigned Opcode, Type *Src,
                                  Align Alignment, unsigned AddressSpace,
                                  TTI::TargetCostKind CostKind,
                                  TTI::OperandValueInfo OpInfo = {TTI::OK_AnyValue, TTI::OP_None},
                                  const Instruction *I = nullptr) const override;

  using BaseT::getVectorInstrCost;
  InstructionCost getVectorInstrCost(unsigned Opcode, Type *Val,
                                     TTI::TargetCostKind CostKind,
                                     unsigned Index, const Value *Op0,
                                     const Value *Op1) const override;

  InstructionCost getShuffleCost(TTI::ShuffleKind Kind, VectorType *DstTy,
                                 VectorType *SrcTy, ArrayRef<int> Mask,
                                 TTI::TargetCostKind CostKind, int Index,
                                 VectorType *SubTp,
                                 ArrayRef<const Value *> Args = {},
                                 const Instruction *CxtI = nullptr) const override;

  InstructionCost getIntrinsicInstrCost(const IntrinsicCostAttributes &ICA,
                                        TTI::TargetCostKind CostKind) const override;

  InstructionCost
  getArithmeticReductionCost(unsigned Opcode, VectorType *Ty,
                             std::optional<FastMathFlags> FMF,
                             TTI::TargetCostKind CostKind) const override;

  InstructionCost
  getMinMaxReductionCost(Intrinsic::ID IID, VectorType *Ty, FastMathFlags FMF,
                         TTI::TargetCostKind CostKind) const override;

  InstructionCost getCastInstrCost(unsigned Opcode, Type *Dst, Type *Src,
                                   TTI::CastContextHint CCH,
                                   TTI::TargetCostKind CostKind,
                                   const Instruction *I = nullptr) const override;

  InstructionCost getCmpSelInstrCost(
      unsigned Opcode, Type *ValTy, Type *CondTy, CmpInst::Predicate VecPred,
      TTI::TargetCostKind CostKind,
      TTI::OperandValueInfo Op1Info = {TTI::OK_AnyValue, TTI::OP_None},
      TTI::OperandValueInfo Op2Info = {TTI::OK_AnyValue, TTI::OP_None},
      const Instruction *I = nullptr) const override;

  InstructionCost getInterleavedMemoryOpCost(
      unsigned Opcode, Type *VecTy, unsigned Factor, ArrayRef<unsigned> Indices,
      Align Alignment, unsigned AddressSpace, TTI::TargetCostKind CostKind,
      bool UseMaskForCond = false, bool UseMaskForGaps = false) const override;

  /// Prefer in-loop reductions for VU0 FP operations.
  /// The FPU accumulator chain (MULA.S/MADDA.S/MADD.S) is more efficient
  /// than vectorizing and then doing expensive horizontal reductions.
  bool preferInLoopReduction(RecurKind Kind, Type *Ty) const override;

  /// \name Vectorizer Tuning
  /// @{

  // Enable interleaved memory access vectorization for R5900.
  // This allows efficient use of LQ/SQ for strided patterns.
  bool enableInterleavedAccessVectorization() const override {
    return ST->hasVU0() || ST->isR5900();
  }

  // Maximum interleave factor for VU0/MMI.
  // VU0 has 32 VF registers, MMI uses GPR128 (32 registers).
  // Conservative factor of 2 balances register pressure vs throughput.
  unsigned getMaxInterleaveFactor(ElementCount VF) const override {
    if (ST->hasVU0() || ST->isR5900())
      return 2;
    return 1;
  }

  /// @}
};

} // end namespace llvm

#endif
