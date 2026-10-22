//===-- MipsTargetTransformInfo.cpp - Mips specific TTI ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MipsTargetTransformInfo.h"
#include "MipsSEISelLowering.h"
#include "MipsSubtarget.h"
#include "llvm/Analysis/IVDescriptors.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/PatternMatch.h"

using namespace llvm;
using namespace llvm::PatternMatch;

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

bool MipsTTIImpl::shouldMaximizeVectorBandwidth(
    TargetTransformInfo::RegisterKind K) const {
  // MMI packs narrow elements: a loop that reads i32 and writes i16 can work
  // on 8 elements and pack them with PPACH into one register to store.
  return ST->isR5900() && K == TargetTransformInfo::RGK_FixedWidthVector;
}

bool MipsTTIImpl::isProfitableToSinkOperands(
    Instruction *I, SmallVectorImpl<Use *> &Ops) const {
  // A v8i32 multiply of values sign extended from i16 is PMULTH. Instruction
  // selection only sees the extensions in the block of the multiply, so sink
  // them, also of a broadcast scalar.
  if (!ST->isR5900() || I->getOpcode() != Instruction::Mul ||
      I->getType() !=
          FixedVectorType::get(Type::getInt32Ty(I->getContext()), 8))
    return false;
  auto IsSExt16 = [](Value *V) {
    auto *SExt = dyn_cast<SExtInst>(V);
    return SExt && SExt->getSrcTy()->getScalarSizeInBits() <= 16;
  };
  for (Use &U : I->operands()) {
    if (IsSExt16(U.get())) {
      Ops.push_back(&U);
      continue;
    }
    Value *Scalar;
    auto *Shuf = dyn_cast<ShuffleVectorInst>(U.get());
    if (!Shuf ||
        !match(Shuf,
               m_Shuffle(m_InsertElt(m_Undef(), m_Value(Scalar), m_ZeroInt()),
                         m_Undef(), m_ZeroMask())) ||
        !IsSExt16(Scalar))
      return false;
    auto *Insert = cast<Instruction>(Shuf->getOperand(0));
    Ops.push_back(&Insert->getOperandUse(1));
    Ops.push_back(&Shuf->getOperandUse(0));
    Ops.push_back(&U);
  }
  return true;
}

// R5900 MMI costs are in issue slots, from the bench uarch tests on hardware.
// The R5900 issues two instructions per cycle, so an ALU operation, a load and
// a store take one slot (half a cycle), like the scalar costs:
//   - an MMI operation (paddw, pcgtw, pmaxw, pand, psllw and the pextlw,
//     ppacw, pcpyld, pcpyud, pexew, pcpyh, prot3w shuffles) has a latency of 1
//     and issues one per cycle. It uses both integer pipes, so it does not pair
//     with an ALU operation, but it does pair with a load or store.
//   - lq and sq issue one per cycle, like the other loads and stores; lq has a
//     latency of 2.
//   - a load of a just stored value does not stall (sq->lw: 3 cycles).
// An MMI operation, and a cycle of the loads and stores of a stack temporary
// that nothing pairs with, take both slots.
// Operations without a single MMI instruction are lowered through a stack
// temporary, so their cost is twice the number of loads and stores.
static constexpr unsigned MMICycle = 2;

// VU0 costs are in the same issue slots, from the bench uarch tests:
//   - a VU0 operation (vadd, vmul, vmax, vabs, vmove), like an FPU operation,
//     has a latency of 4 and issues one per cycle. It pairs with an ALU
//     operation or a load, so it takes one slot, as do lqc2 and sqc2.
//   - qmfc2, qmtc2 and mtc1 issue one per cycle and do not pair with each
//     other; qmfc2 to a GPR use is 3 cycles, qmfc2 and mtc1 to an FPU use 4.5.
// Moving an element between VU0 and the FPU is such a chain of moves, so
// each move takes both slots, as do the MMI operations that build a vector
// from its elements in a GPR (pextlw, pcpyld) or take one apart (pcpyud).

/// Whether \p Ty is the v8i32 that PMULTH multiplies.
static bool isPMULTHType(Type *Ty) {
  return Ty == FixedVectorType::get(Type::getInt32Ty(Ty->getContext()), 8);
}

/// Whether \p V is an operand of a v8i32 multiply that PMULTH takes as a
/// v8i16: a value sign extended from i16 or smaller, or a 16-bit constant.
static bool isPMULTHOperand(const Value *V) {
  if (auto *C = dyn_cast<ConstantInt>(V))
    return C->getValue().isSignedIntN(16);
  auto *SExt = dyn_cast<SExtInst>(V);
  return SExt && SExt->getSrcTy()->getScalarSizeInBits() <= 16;
}

std::optional<std::pair<InstructionCost, MVT>>
MipsTTIImpl::getMMIType(Type *Ty) const {
  // Smaller vectors, such as v8i8, are promoted to a 128-bit MMI type.
  auto *VTy = dyn_cast<FixedVectorType>(Ty);
  if (!ST->isR5900() || !VTy || !VTy->getElementType()->isIntegerTy() ||
      (VTy->getPrimitiveSizeInBits() > 128 &&
       VTy->getPrimitiveSizeInBits() % 128 != 0))
    return std::nullopt;
  std::pair<InstructionCost, MVT> LT = getTypeLegalizationCost(VTy);
  if (!LT.second.isVector() || LT.second.getSizeInBits() != 128)
    return std::nullopt;
  return LT;
}

std::optional<unsigned> MipsTTIImpl::getVU0Parts(Type *Ty) const {
  auto *VTy = dyn_cast<FixedVectorType>(Ty);
  if (!ST->hasVU0() || !VTy || !VTy->getElementType()->isFloatTy())
    return std::nullopt;
  return divideCeil(VTy->getNumElements(), 4);
}

InstructionCost
MipsTTIImpl::getR5900ScalarizedCost(FixedVectorType *VTy, unsigned NumVecOps,
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
  // Per v4f32 part: extracting is a qmfc2, a pcpyud for the upper two
  // elements, a dsrl32 for an odd one and an mtc1 per element. Inserting is
  // an mfc1 per element, a pextlw for an odd one, a pcpyld for the upper two
  // and a qmtc2.
  if (auto Parts = getVU0Parts(Ty)) {
    InstructionCost Cost = 0;
    for (unsigned Part = 0; Part < *Parts; ++Part) {
      unsigned Demanded = 0, Odd = 0;
      bool Upper = false;
      for (unsigned I = 0; I < 4 && Part * 4 + I < DemandedElts.getBitWidth();
           ++I) {
        if (!DemandedElts[Part * 4 + I])
          continue;
        ++Demanded;
        Odd += I & 1;
        Upper |= I >= 2;
      }
      if (!Demanded)
        continue;
      if (Extract)
        Cost += (1 + Upper + Demanded) * MMICycle + Odd;
      if (Insert)
        Cost += (Demanded + Odd + Upper + 1) * MMICycle;
    }
    return Cost;
  }

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
  // A vector smaller than its legal type, such as v2i8 widened to v16i8, has
  // fewer elements than a part.
  for (unsigned Part = 0; Part < NumElts; Part += PartElts) {
    unsigned End = std::min(Part + PartElts, NumElts);
    unsigned Demanded = 0;
    for (unsigned I = Part; I < End; ++I)
      Demanded += DemandedElts[I];
    if (!Demanded)
      continue;
    if (Extract)
      Cost += 1 + Demanded;
    if (Insert)
      Cost += Demanded + 1 + (!ForPoisonSrc && Demanded != End - Part);
  }
  return Cost * MMICycle;
}

InstructionCost MipsTTIImpl::getArithmeticInstrCost(
    unsigned Opcode, Type *Ty, TTI::TargetCostKind CostKind,
    TTI::OperandValueInfo Op1Info, TTI::OperandValueInfo Op2Info,
    ArrayRef<const Value *> Args, const Instruction *CxtI) const {

  if (auto Parts = getVU0Parts(Ty)) {
    switch (Opcode) {
    case Instruction::FAdd:
    case Instruction::FSub:
    case Instruction::FMul:
      return *Parts;
    case Instruction::FNeg:
      // qmfc2, pxor with the sign bits and qmtc2.
      return *Parts * 4 * MMICycle;
    default:
      break;
    }
    // Everything else, such as fdiv, is scalarized.
    auto *VTy = cast<FixedVectorType>(Ty);
    InstructionCost ScalarCost =
        getArithmeticInstrCost(Opcode, VTy->getElementType(), CostKind,
                               Op1Info.getNoProps(), Op2Info.getNoProps());
    unsigned NumVecOps =
        Opcode == Instruction::FNeg || Op2Info.isConstant() ? 1 : 2;
    return getR5900ScalarizedCost(VTy, NumVecOps, ScalarCost, CostKind);
  }

  // An FPU add, subtract or multiply takes one slot, like the VU0 ones.
  if (ST->isR5900() && Ty->isFloatTy() &&
      (Opcode == Instruction::FAdd || Opcode == Instruction::FSub ||
       Opcode == Instruction::FMul))
    return 1;

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
        return LT->first * MMICycle;
      break;
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:
      // psllh/w, psrlh/w, psrah/w only take an immediate shift amount.
      if ((EltBits == 16 || EltBits == 32) && Op2Info.isUniform() &&
          Op2Info.isConstant())
        return LT->first * MMICycle;
      break;
    case Instruction::Mul:
      // A v8i32 multiply of values sign extended from i16 is PMULTH, then
      // PMFLO, PMFHI, PCPYLD and PCPYUD for the products in order.
      if (isPMULTHType(Ty) && Args.size() == 2 && all_of(Args, isPMULTHOperand))
        return 5 * MMICycle;
      break;
    default:
      break;
    }
    // Everything else, including other multiplies, is scalarized.
    auto *VTy = cast<FixedVectorType>(Ty);
    InstructionCost ScalarCost =
        getArithmeticInstrCost(Opcode, VTy->getElementType(), CostKind,
                               Op1Info.getNoProps(), Op2Info.getNoProps());
    unsigned NumVecOps = Op2Info.isConstant() ? 1 : 2;
    return getR5900ScalarizedCost(VTy, NumVecOps, ScalarCost, CostKind);
  }

  return BaseT::getArithmeticInstrCost(Opcode, Ty, CostKind, Op1Info, Op2Info,
                                       Args, CxtI);
}

/// Whether the loop vectorizer's access of type \p VecTy that widens the
/// scalar load or store \p I is 16-byte aligned, although \p I is not: \p I
/// accesses consecutive elements of a 16-byte aligned object, from an
/// induction variable that starts at a multiple of 16 bytes.
static bool isAlignedWhenWidened(const Instruction *I, Type *VecTy,
                                 const DataLayout &DL) {
  if (!I || !isa<LoadInst, StoreInst>(I) ||
      getLoadStoreType(const_cast<Instruction *>(I))->isVectorTy() ||
      VecTy->getPrimitiveSizeInBits().getFixedValue() % 128)
    return false;
  // A constant offset from a GEP whose last index is the induction variable
  // and the others are constants, as in a[i], a[2][i] or &a[i] + 512.
  const Value *Ptr = getLoadStorePointerOperand(I);
  APInt Offset(DL.getIndexTypeSizeInBits(Ptr->getType()), 0);
  auto *GEP = dyn_cast<GetElementPtrInst>(
      Ptr->stripAndAccumulateConstantOffsets(DL, Offset,
                                             /*AllowNonInbounds=*/true));
  if (!GEP || GEP->getNumIndices() == 0)
    return false;
  Type *EltTy = nullptr;
  for (gep_type_iterator GTI = gep_type_begin(GEP), E = gep_type_end(GEP);
       GTI != E; ++GTI) {
    if (std::next(GTI) == E) {
      if (GTI.isStruct())
        return false;
      EltTy = GTI.getIndexedType();
      break;
    }
    auto *C = dyn_cast<ConstantInt>(GTI.getOperand());
    if (!C)
      return false;
    if (StructType *STy = GTI.getStructTypeOrNull())
      Offset += DL.getStructLayout(STy)->getElementOffset(C->getZExtValue());
    else
      Offset += C->getValue().sextOrTrunc(Offset.getBitWidth()) *
                GTI.getSequentialElementStride(DL);
  }
  if (DL.getTypeAllocSize(EltTy) !=
      DL.getTypeStoreSize(getLoadStoreType(const_cast<Instruction *>(I))))
    return false;

  const Value *Base =
      GEP->getPointerOperand()->stripAndAccumulateConstantOffsets(
          DL, Offset, /*AllowNonInbounds=*/true);
  if (commonAlignment(Base->getPointerAlignment(DL), Offset.getZExtValue()) <
      Align(16))
    return false;

  // The index starts at a multiple of 16 bytes and steps by one element.
  auto *Phi = dyn_cast<PHINode>(GEP->getOperand(GEP->getNumOperands() - 1));
  if (!Phi || Phi->getNumIncomingValues() != 2)
    return false;
  uint64_t EltSize = DL.getTypeAllocSize(EltTy);
  bool Start = false, Step = false;
  for (const Value *V : Phi->incoming_values()) {
    if (const auto *C = dyn_cast<ConstantInt>(V))
      Start = C->getZExtValue() * EltSize % 16 == 0;
    else if (const auto *Add = dyn_cast<BinaryOperator>(V))
      Step = Add->getOpcode() == Instruction::Add &&
             Add->getOperand(0) == Phi && match(Add->getOperand(1), m_One());
  }
  return Start && Step;
}

InstructionCost MipsTTIImpl::getMemoryOpCost(unsigned Opcode, Type *Src,
                                             Align Alignment,
                                             unsigned AddressSpace,
                                             TTI::TargetCostKind CostKind,
                                             TTI::OperandValueInfo OpInfo,
                                             const Instruction *I) const {
  // VU0 v4f32 load/store: an aligned lqc2/sqc2 takes 1 slot. Otherwise a
  // load is ld/sd x2, or ldl/ldr and sd x2, to a stack temporary and an
  // lqc2; a store an sqc2, then ld/sd x2, or ld and sdl/sdr x2. A vector
  // that is not a multiple of v4f32, such as v2f32, goes element by element
  // through stack temporaries and the FPU.
  if (auto Parts = getVU0Parts(Src)) {
    unsigned NumElts = cast<FixedVectorType>(Src)->getNumElements();
    if (NumElts % 4)
      return (2 * NumElts + 5) * MMICycle;
    if (Alignment >= Align(16) || isAlignedWhenWidened(I, Src, DL))
      return *Parts;
    return *Parts * (Alignment >= Align(8) ? 5 : 7) * MMICycle;
  }

  // A smaller integer vector, such as v8i8, is promoted to a 128-bit MMI type
  // with wider elements. Its load goes through a stack temporary: a load and a
  // store of each element, then lq; its store the other way around. Loads and
  // stores issue one per cycle.
  if (auto *VTy = dyn_cast<FixedVectorType>(Src);
      ST->isR5900() && VTy && VTy->getElementType()->isIntegerTy()) {
    MVT LT = getTypeLegalizationCost(VTy).second;
    if (LT.isVector() && LT.getSizeInBits() == 128 &&
        LT.getScalarSizeInBits() != VTy->getScalarSizeInBits())
      return (2 * VTy->getNumElements() + 1) * MMICycle;
  }

  // R5900 MMI integer vector load/store, per 128-bit part: an aligned lq/sq
  // takes 1 slot. An unaligned load is ldl/ldr x2 and pcpyld, 5 cycles to the
  // use; an unaligned store pcpyud and sdl/sdr x2, 4 cycles.
  if (auto LT = getMMIType(Src)) {
    if (Alignment >= Align(16) || isAlignedWhenWidened(I, Src, DL))
      return LT->first;
    return LT->first * (Opcode == Instruction::Load ? 5 : 4) * MMICycle;
  }

  return BaseT::getMemoryOpCost(Opcode, Src, Alignment, AddressSpace, CostKind,
                                OpInfo, I);
}

InstructionCost MipsTTIImpl::getVectorInstrCost(
    unsigned Opcode, Type *Val, TTI::TargetCostKind CostKind, unsigned Index,
    const Value *Op0, const Value *Op1, TTI::VectorInstrContext VIC) const {
  // VU0 v4f32 extract: qmfc2, then pcpyud for the upper two elements and a
  // dsrl32 for an odd one, and mtc1. Insert: a vector with only that element
  // built as above, then a vmove of the element unless the vector is
  // undefined. A variable element goes through a stack temporary.
  if (getVU0Parts(Val)) {
    if (Index == -1U)
      return (Opcode == Instruction::ExtractElement ? 3 : 4) * MMICycle;
    unsigned Elt = Index % 4;
    if (Opcode == Instruction::ExtractElement)
      return (2 + (Elt >= 2)) * MMICycle + (Elt & 1);
    if (Opcode == Instruction::InsertElement)
      return (2 + (Elt & 1) + (Elt >= 2)) * MMICycle +
             !(Op0 && isa<UndefValue>(Op0));
  }

  // R5900 MMI integer vector extract/insert, through a stack temporary.
  if (getMMIType(Val)) {
    // sq, then a load of the element.
    if (Opcode == Instruction::ExtractElement)
      return 2 * MMICycle;
    if (Opcode == Instruction::InsertElement) {
      // Into an undefined vector: a store of the element and an lq.
      if (Op0 && isa<UndefValue>(Op0))
        return 2 * MMICycle;
      // Otherwise the element is merged in memory: 12 loads and stores.
      return 12 * MMICycle;
    }
  }

  return BaseT::getVectorInstrCost(Opcode, Val, CostKind, Index, Op0, Op1, VIC);
}

InstructionCost MipsTTIImpl::getShuffleCost(
    TTI::ShuffleKind Kind, VectorType *DstTy, VectorType *SrcTy,
    TTI::TargetCostKind CostKind, ArrayRef<int> Mask, int Index,
    VectorType *SubTp, ArrayRef<const Value *> Args, const Instruction *CxtI,
    TTI::VectorInstrContext VIC) const {
  // VU0 v4f32 shuffles: vmove with a field mask blends two vectors, and vsub
  // and vadd with a broadcast field splat an element. The others are a
  // qmfc2 of each source, about four MMI or shift operations and a qmtc2.
  if (getVU0Parts(DstTy) == 1u && (!SrcTy || getVU0Parts(SrcTy) == 1u)) {
    unsigned NumElts = cast<FixedVectorType>(DstTy)->getNumElements();
    int First = -1;
    bool Splat = true, Blend = true;
    for (auto [I, M] : enumerate(Mask)) {
      if (M < 0)
        continue;
      if (First < 0)
        First = M;
      Splat &= M == First;
      Blend &= unsigned(M) % NumElts == I;
    }
    if (Kind == TTI::SK_Select || (First >= 0 && Blend))
      return 1;
    if (Kind == TTI::SK_Broadcast || (First >= 0 && Splat))
      return 2;
    unsigned NumSrcs = Kind == TTI::SK_PermuteTwoSrc ||
                               Kind == TTI::SK_Transpose ||
                               Kind == TTI::SK_Splice
                           ? 2
                           : 1;
    return (NumSrcs + 5) * MMICycle;
  }

  // R5900 MMI integer vector shuffles: the zips, unzips and half copies of
  // one 128-bit vector are MMI instructions. The others are lowered through a
  // stack temporary: per 128-bit result part, an sq per source, a load and a
  // store per element and an lq. A broadcast loads the element once.
  if (auto LT = getMMIType(DstTy)) {
    unsigned PartElts = LT->second.getVectorNumElements();
    if (LT->first == 1 && Mask.size() == PartElts) {
      bool SingleSrc = all_of(Mask, [&](int M) { return M < (int)PartElts; });
      if (unsigned Ops = getR5900ShuffleOps(Mask, PartElts, SingleSrc))
        return Ops * MMICycle;
    }
    if (Kind == TTI::SK_Broadcast)
      return (LT->first * (PartElts + 1) + 2) * MMICycle;
    unsigned NumSrcs =
        Kind == TTI::SK_PermuteTwoSrc || Kind == TTI::SK_Select ||
                Kind == TTI::SK_Transpose || Kind == TTI::SK_Splice
            ? 2
            : 1;
    return LT->first * (NumSrcs + 2 * PartElts + 1) * MMICycle;
  }

  return BaseT::getShuffleCost(Kind, DstTy, SrcTy, CostKind, Mask, Index, SubTp,
                               Args, CxtI, VIC);
}

InstructionCost
MipsTTIImpl::getIntrinsicInstrCost(const IntrinsicCostAttributes &ICA,
                                   TTI::TargetCostKind CostKind) const {
  Type *RetTy = ICA.getReturnType();

  // VU0 v4f32 intrinsics: vabs, vmini, vmax, and vmul and vadd for a
  // multiply-add. The others, such as sqrt, are scalarized.
  if (auto Parts = getVU0Parts(RetTy)) {
    switch (ICA.getID()) {
    case Intrinsic::fabs:
    case Intrinsic::minnum:
    case Intrinsic::maxnum:
      return *Parts;
    case Intrinsic::fma:
    case Intrinsic::fmuladd:
      return 2 * *Parts;
    default:
      break;
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
        return LT->first * MMICycle;
      break;
    case Intrinsic::sadd_sat:
    case Intrinsic::ssub_sat:
    case Intrinsic::uadd_sat:
    case Intrinsic::usub_sat:
      // paddsb/h/w, psubsb/h/w, paddub/h/w, psubub/h/w
      if (EltBits != 64)
        return LT->first * MMICycle;
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

  // VU0 v4f32 fadd and fmul reductions: the upper half goes through the FPU
  // and a stack temporary, then two VU0 operations and qmfc2 and mtc1 for the
  // result, a chain of about 14 cycles.
  if (getVU0Parts(Ty) == 1u)
    return 14 * MMICycle;

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

  // VU0 v4f32 min/max reductions are lowered like the fadd ones.
  if (getVU0Parts(Ty) == 1u)
    return 14 * MMICycle;

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
    // PPACH and PPACB pack two registers into a v8i16 or v16i8, one per pair
    // of source registers.
    if (ST->isR5900() && DstBits == 128 && SrcBits > 128 &&
        SrcVTy->getScalarSizeInBits() <= 32 &&
        (DstVTy->getScalarSizeInBits() == 16 ||
         DstVTy->getScalarSizeInBits() == 8))
      return (SrcBits / 128 - 1) * MMICycle;
    [[fallthrough]];
  case Instruction::ZExt:
  case Instruction::SExt:
    // PMULTH takes the v8i16 itself, so a sign extension that only feeds
    // such multiplies is free.
    if (ST->isR5900() && Opcode == Instruction::SExt && I &&
        isPMULTHType(DstVTy) && isPMULTHOperand(I) && !I->use_empty() &&
        all_of(I->users(), [](const User *U) {
          auto *Mul = dyn_cast<BinaryOperator>(U);
          return Mul && Mul->getOpcode() == Instruction::Mul &&
                 all_of(Mul->operands(), isPMULTHOperand);
        }))
      return 0;
    // PEXTLH/PEXTUH and PEXTLB/PEXTUB extend a v8i16 or v16i8 to two
    // registers of elements twice as wide, a sign extension with an
    // arithmetic shift of each; bytes to words in two steps.
    if (ST->isR5900() && Opcode != Instruction::Trunc && SrcBits == 128 &&
        DstBits > 128 && DstVTy->getScalarSizeInBits() <= 32 &&
        (SrcVTy->getScalarSizeInBits() == 16 ||
         SrcVTy->getScalarSizeInBits() == 8)) {
      unsigned PerPart = Opcode == Instruction::SExt ? 2 : 1;
      unsigned Parts = DstBits / 128;
      // Bytes to words also extends the two halves to halfwords first.
      unsigned Ops = Parts == 4 ? 2 * PerPart + 4 * PerPart : 2 * PerPart;
      return Ops * MMICycle;
    }
    // Other MMI integer vector extensions and truncations are scalarized.
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
    // VU0 v4f32 to and from v4i32: vftoi0 and qmfc2, or qmtc2 and vitof0.
    // An unsigned conversion to v4i32 also converts X - 2^31 and selects
    // with MMI operations; one from v4i32 converts both halfwords.
    if (auto Parts = getVU0Parts(Opcode == Instruction::FPToSI ||
                                         Opcode == Instruction::FPToUI
                                     ? Src
                                     : Dst);
        Parts && DstVTy->getNumElements() % 4 == 0 &&
        DstVTy->getScalarSizeInBits() == 32 &&
        SrcVTy->getScalarSizeInBits() == 32) {
      switch (Opcode) {
      case Instruction::FPToSI:
      case Instruction::SIToFP:
        return *Parts * (1 + MMICycle);
      case Instruction::FPToUI:
        return *Parts * (3 + 7 * MMICycle);
      default:
        return *Parts * (4 + 6 * MMICycle);
      }
    }
    // Other VU0 v4f32 conversions are scalarized.
    if (getVU0Parts(Src) || getVU0Parts(Dst)) {
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

  // MMI compares are PCGT and PCEQ, with the operands swapped, the mask
  // inverted (PNOR) or the sign bits of the operands flipped (PXOR) for the
  // other predicates. A select with a mask is PXOR, PAND and PXOR.
  auto LT = getMMIType(VTy);
  if (LT && LT->second.getScalarSizeInBits() != 64) {
    if (Opcode == Instruction::Select && CondTy && CondTy->isVectorTy())
      return LT->first * 3 * MMICycle;
    if (Opcode == Instruction::ICmp) {
      unsigned NumOps = 1;
      if (ICmpInst::isUnsigned(VecPred))
        NumOps += 2;
      if (VecPred == ICmpInst::ICMP_NE || VecPred == ICmpInst::ICMP_SGE ||
          VecPred == ICmpInst::ICMP_SLE || VecPred == ICmpInst::ICMP_UGE ||
          VecPred == ICmpInst::ICMP_ULE)
        NumOps += 1;
      return LT->first * NumOps * MMICycle;
    }
  }

  // Other MMI integer vector compares and selects are scalarized.
  if ((Opcode == Instruction::ICmp || Opcode == Instruction::Select) && LT) {
    InstructionCost ScalarCost = getCmpSelInstrCost(
        Opcode, EltTy, CondTy ? CondTy->getScalarType() : nullptr, VecPred,
        CostKind, Op1Info.getNoProps(), Op2Info.getNoProps());
    return getR5900ScalarizedCost(VTy, 2, ScalarCost, CostKind);
  }

  // VU0 v4f32 compares are scalarized, each a c.cond.s and a branch over a
  // li, to an MMI mask. A select with a mask is qmfc2 of both operands, pand,
  // pnor, pand, por and qmtc2.
  if (auto Parts = getVU0Parts(VTy)) {
    if (Opcode == Instruction::FCmp) {
      auto *MaskTy = VectorType::getInteger(VTy);
      return 2 * getScalarizationOverhead(VTy, /*Insert=*/false,
                                          /*Extract=*/true, CostKind) +
             NumElts * 3 * MMICycle +
             getScalarizationOverhead(MaskTy, /*Insert=*/true,
                                      /*Extract=*/false, CostKind);
    }
    if (Opcode == Instruction::Select && CondTy && CondTy->isVectorTy())
      return *Parts * 7 * MMICycle;
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

  // MMI integer vectors of two members of whole 128-bit registers: the wide
  // load or store and, per register of a member, PPACW/H/B for the even
  // elements, two shifts or exchanges and a pack for the odd ones, or
  // PEXTLx and PEXTUx to interleave a register of each member.
  auto *VTy = dyn_cast<FixedVectorType>(VecTy);
  if (VTy && ST->isR5900() && getMMIType(VTy) && Factor == 2 &&
      !UseMaskForCond && !UseMaskForGaps) {
    auto *MemberTy = FixedVectorType::get(VTy->getElementType(),
                                          VTy->getNumElements() / Factor);
    auto LT = getMMIType(MemberTy);
    if (LT && MemberTy->getPrimitiveSizeInBits() % 128 == 0 &&
        LT->second.getScalarSizeInBits() == MemberTy->getScalarSizeInBits()) {
      InstructionCost Cost =
          getMemoryOpCost(Opcode, VTy, Alignment, AddressSpace, CostKind);
      InstructionCost Ops = 0;
      if (Opcode == Instruction::Store)
        Ops = 2 * LT->first;
      else if (Indices.empty())
        Ops = 4 * LT->first;
      else
        for (unsigned Index : Indices)
          Ops += (Index == 0 ? 1 : 3) * LT->first;
      return Cost + Ops * MMICycle;
    }
  }

  // Everything else, including VU0 vectors, from the load/store and shuffle
  // costs.
  return BaseT::getInterleavedMemoryOpCost(Opcode, VecTy, Factor, Indices,
                                           Alignment, AddressSpace, CostKind,
                                           UseMaskForCond, UseMaskForGaps);
}
