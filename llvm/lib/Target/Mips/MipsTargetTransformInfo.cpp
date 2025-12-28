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
// R5900 VU0 Vectorization Support
//===----------------------------------------------------------------------===//

unsigned MipsTTIImpl::getNumberOfRegisters(unsigned ClassID) const {
  bool Vector = (ClassID == 1);
  if (Vector && ST->hasVU0())
    return 32; // VF0-VF31 (though VF0 is a constant register)
  return 32; // GP registers
}

TypeSize
MipsTTIImpl::getRegisterBitWidth(TargetTransformInfo::RegisterKind K) const {
  switch (K) {
  case TargetTransformInfo::RGK_Scalar:
    return TypeSize::getFixed(ST->isGP64bit() ? 64 : 32);
  case TargetTransformInfo::RGK_FixedWidthVector:
    if (ST->hasVU0())
      return TypeSize::getFixed(128); // VU0 has 128-bit VF registers
    return TypeSize::getFixed(0);
  case TargetTransformInfo::RGK_ScalableVector:
    return TypeSize::getScalable(0); // No scalable vector support
  }
  llvm_unreachable("Unsupported register kind");
}

unsigned MipsTTIImpl::getMinVectorRegisterBitWidth() const {
  return ST->hasVU0() ? 128 : 0;
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
        return 1; // Native VU0 operation
      default:
        break;
      }
    }
  }
  return BaseT::getArithmeticInstrCost(Opcode, Ty, CostKind, Op1Info, Op2Info,
                                       Args, CxtI);
}
