//===-- VULowerMemory.cpp - Loads and stores in VU data memory ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Pointers are byte addresses of VU data memory, but the VU addresses it in
// quadwords and has no shifter to divide an address by 16. This pass rewrites
// plain loads and stores into the llvm.vu.* memory intrinsics: the address is
// split into a sum of terms (value * scale) and a constant, where every scale
// is a multiple of 16 bytes, so the quadword address is the same sum with the
// scales divided by 16. The rest of the constant selects the fields:
//
//   %p = getelementptr <4 x float>, ptr inttoptr (i16 32 to ptr), i16 %i
//   %f = getelementptr i8, ptr %p, i16 8
//   load float, ptr %f
//     ->
//   %q = add i16 %i, 2
//   %v = call <4 x float> @llvm.vu.lq(i16 %q)
//   extractelement <4 x float> %v, i32 2
//
// A value is stored with the masked SQ, so the other fields of a quadword
// keep their values. 16-bit integers are the low half of a field (ILW, ISW).
//
//===----------------------------------------------------------------------===//

#include "VU.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsVU.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;
using namespace llvm::PatternMatch;

#define DEBUG_TYPE "vu-lower-memory"

namespace {
/// A byte address: the sum of Value * Scale for the terms, plus Const.
struct Address {
  MapVector<Value *, int64_t> Terms;
  int64_t Const = 0;
};

class VULowerMemory : public FunctionPass {
public:
  static char ID;
  VULowerMemory() : FunctionPass(ID) {}

  StringRef getPassName() const override { return "VU lower memory accesses"; }

  bool runOnFunction(Function &F) override;

private:
  const DataLayout *DL = nullptr;

  bool decomposePointer(Value *P, int64_t Scale, Address &A);
  void decomposeInt(Value *V, int64_t Scale, Address &A);
  Value *getQuadword(IRBuilder<> &B, const Address &A, int64_t Quad);
  bool lowerLoad(LoadInst &LI);
  bool lowerStore(StoreInst &SI);
};
} // namespace

char VULowerMemory::ID = 0;

INITIALIZE_PASS(VULowerMemory, DEBUG_TYPE, "VU lower memory accesses", false,
                false)

FunctionPass *llvm::createVULowerMemoryPass() { return new VULowerMemory(); }

void VULowerMemory::decomposeInt(Value *V, int64_t Scale, Address &A) {
  if (auto *C = dyn_cast<ConstantInt>(V)) {
    A.Const += C->getSExtValue() * Scale;
    return;
  }
  Value *X, *Y;
  const APInt *C;
  if (match(V, m_Add(m_Value(X), m_Value(Y))) ||
      match(V, m_DisjointOr(m_Value(X), m_Value(Y)))) {
    decomposeInt(X, Scale, A);
    decomposeInt(Y, Scale, A);
    return;
  }
  if (match(V, m_Sub(m_Value(X), m_Value(Y)))) {
    decomposeInt(X, Scale, A);
    decomposeInt(Y, -Scale, A);
    return;
  }
  if (match(V, m_Mul(m_Value(X), m_APInt(C))) && C->getSignificantBits() < 32) {
    decomposeInt(X, Scale * C->getSExtValue(), A);
    return;
  }
  if (match(V, m_Shl(m_Value(X), m_APInt(C))) && C->ult(16)) {
    decomposeInt(X, Scale << C->getZExtValue(), A);
    return;
  }
  // Addresses are 16 bits: the low bits of a wider sum are the sum of the
  // low bits.
  if (match(V, m_Trunc(m_Value(X)))) {
    decomposeInt(X, Scale, A);
    return;
  }
  A.Terms[V] += Scale;
}

bool VULowerMemory::decomposePointer(Value *P, int64_t Scale, Address &A) {
  if (isa<ConstantPointerNull>(P))
    return true;
  if (auto *GEP = dyn_cast<GEPOperator>(P)) {
    if (!decomposePointer(GEP->getPointerOperand(), Scale, A))
      return false;
    for (gep_type_iterator GTI = gep_type_begin(GEP), E = gep_type_end(GEP);
         GTI != E; ++GTI) {
      Value *Idx = GTI.getOperand();
      if (StructType *ST = GTI.getStructTypeOrNull()) {
        unsigned Field = cast<ConstantInt>(Idx)->getZExtValue();
        A.Const += DL->getStructLayout(ST)->getElementOffset(Field) * Scale;
        continue;
      }
      int64_t Size = GTI.getSequentialElementStride(*DL);
      decomposeInt(Idx, Size * Scale, A);
    }
    return true;
  }
  if (auto *O = dyn_cast<Operator>(P);
      O && O->getOpcode() == Instruction::AddrSpaceCast)
    return decomposePointer(O->getOperand(0), Scale, A);
  if (auto *O = dyn_cast<Operator>(P);
      O && O->getOpcode() == Instruction::IntToPtr) {
    decomposeInt(O->getOperand(0), Scale, A);
    return true;
  }
  return false;
}

/// The quadword address of byte address \p A plus \p Quad quadwords.
Value *VULowerMemory::getQuadword(IRBuilder<> &B, const Address &A,
                                  int64_t Quad) {
  Type *I16 = B.getInt16Ty();
  Value *Q = B.getInt16(A.Const / 16 + Quad);
  for (auto [V, Scale] : A.Terms) {
    if (!Scale)
      continue;
    Value *T = B.CreateSExtOrTrunc(V, I16);
    if (Scale != 16)
      T = B.CreateMul(T, ConstantInt::getSigned(I16, Scale / 16));
    Q = B.CreateAdd(T, Q);
  }
  return Q;
}

/// The element type of a scalar or vector value and its number of elements,
/// if it is made of 32-bit floats or integers.
static std::optional<std::pair<Type *, unsigned>> getElements(Type *T) {
  unsigned N = 1;
  if (auto *VT = dyn_cast<FixedVectorType>(T)) {
    N = VT->getNumElements();
    T = VT->getElementType();
  }
  if (!T->isFloatTy() && !T->isIntegerTy(32))
    return std::nullopt;
  return std::make_pair(T, N);
}

static void diagnose(Instruction &I, const Twine &Msg) {
  Function &F = *I.getFunction();
  F.getContext().diagnose(
      DiagnosticInfoUnsupported(F, Msg, I.getDebugLoc(), DS_Error));
}

static bool normalizeConst(Address &A, Instruction &I) {
  for (auto [V, Scale] : A.Terms)
    if (Scale % 16) {
      diagnose(I, "VU: memory access that is not by quadwords");
      return false;
    }
  return true;
}

static void copyAAMetadata(Instruction &From, Value *To) {
  cast<Instruction>(To)->copyMetadata(From, {LLVMContext::MD_alias_scope,
                                             LLVMContext::MD_noalias,
                                             LLVMContext::MD_tbaa});
}

bool VULowerMemory::lowerLoad(LoadInst &LI) {
  Address A;
  if (LI.isVolatile() || LI.isAtomic() ||
      !decomposePointer(LI.getPointerOperand(), 1, A)) {
    diagnose(LI, "VU: unsupported memory access");
    return false;
  }
  if (!normalizeConst(A, LI))
    return false;
  IRBuilder<> B(&LI);
  // The constant split into whole quadwords and the byte in the quadword.
  int64_t Sub = ((A.Const % 16) + 16) % 16;
  A.Const -= Sub;
  Type *T = LI.getType();

  if (T->isIntegerTy(16)) {
    if (Sub % 4) {
      diagnose(LI, "VU: 16-bit load from the high half of a field");
      return false;
    }
    Value *C = B.CreateIntrinsic(Intrinsic::vu_ilw, {},
                                 {getQuadword(B, A, 0), B.getInt32(Sub / 4)});
    copyAAMetadata(LI, C);
    LI.replaceAllUsesWith(C);
    return true;
  }

  auto Elts = getElements(T);
  if (!Elts || Sub % 4) {
    diagnose(LI, "VU: unsupported load type");
    return false;
  }
  auto [EltTy, N] = *Elts;
  Type *QuadTy = FixedVectorType::get(EltTy, 4);
  // One LQ for each quadword that holds an element.
  DenseMap<int64_t, Value *> Quads;
  auto GetQuad = [&](int64_t Q) {
    Value *&V = Quads[Q];
    if (!V) {
      Value *C =
          B.CreateIntrinsic(Intrinsic::vu_lq, {}, {getQuadword(B, A, Q)});
      copyAAMetadata(LI, C);
      V = B.CreateBitCast(C, QuadTy);
    }
    return V;
  };
  Value *Res;
  if (N == 4 && Sub == 0) {
    Res = GetQuad(0);
  } else if (!T->isVectorTy()) {
    Res = B.CreateExtractElement(GetQuad(Sub / 16), Sub / 4 % 4);
  } else if (Sub % 16 + 4 * N <= 16) {
    // The fields of one quadword: a shuffle, nothing from field x.
    SmallVector<int, 4> Mask;
    for (unsigned I = 0; I != N; ++I)
      Mask.push_back(Sub % 16 / 4 + I);
    Res = B.CreateShuffleVector(GetQuad(Sub / 16), Mask);
  } else {
    Res = PoisonValue::get(T);
    for (unsigned I = 0; I != N; ++I) {
      int64_t Byte = Sub + 4 * I;
      Res = B.CreateInsertElement(
          Res, B.CreateExtractElement(GetQuad(Byte / 16), Byte / 4 % 4), I);
    }
  }
  LI.replaceAllUsesWith(Res);
  return true;
}

bool VULowerMemory::lowerStore(StoreInst &SI) {
  Address A;
  if (SI.isVolatile() || SI.isAtomic() ||
      !decomposePointer(SI.getPointerOperand(), 1, A)) {
    diagnose(SI, "VU: unsupported memory access");
    return false;
  }
  if (!normalizeConst(A, SI))
    return false;
  IRBuilder<> B(&SI);
  int64_t Sub = ((A.Const % 16) + 16) % 16;
  A.Const -= Sub;
  Value *V = SI.getValueOperand();
  Type *T = V->getType();

  if (T->isIntegerTy(16)) {
    if (Sub % 4) {
      diagnose(SI, "VU: 16-bit store to the high half of a field");
      return false;
    }
    Value *C = B.CreateIntrinsic(
        Intrinsic::vu_isw, {}, {V, getQuadword(B, A, 0), B.getInt32(Sub / 4)});
    copyAAMetadata(SI, C);
    return true;
  }

  auto Elts = getElements(T);
  if (!Elts || Sub % 4) {
    diagnose(SI, "VU: unsupported store type");
    return false;
  }
  auto [EltTy, N] = *Elts;
  Type *QuadTy = FixedVectorType::get(EltTy, 4);
  Type *F4 = FixedVectorType::get(B.getFloatTy(), 4);
  // The elements of each quadword in their fields, and the mask of these
  // fields (x = 8 ... w = 1).
  MapVector<int64_t, std::pair<Value *, unsigned>> Quads;
  if (N == 4 && Sub == 0) {
    Quads[0] = {V, 15};
  } else if (T->isVectorTy() && Sub % 16 + 4 * N <= 16) {
    // The fields of one quadword: a shuffle into their places.
    unsigned F = Sub % 16 / 4;
    SmallVector<int, 4> Mask(4, -1);
    unsigned FieldMask = 0;
    for (unsigned I = 0; I != N; ++I) {
      Mask[F + I] = I;
      FieldMask |= 8 >> (F + I);
    }
    Quads[Sub / 16] = {B.CreateShuffleVector(V, Mask), FieldMask};
  } else {
    for (unsigned I = 0; I != N; ++I) {
      int64_t Byte = Sub + 4 * I;
      auto &[Q, Mask] = Quads[Byte / 16];
      if (!Q)
        Q = PoisonValue::get(QuadTy);
      Value *E = T->isVectorTy() ? B.CreateExtractElement(V, I) : V;
      Q = B.CreateInsertElement(Q, E, Byte / 4 % 4);
      Mask |= 8 >> (Byte / 4 % 4);
    }
  }
  for (auto &[Quad, QM] : Quads) {
    auto [Q, Mask] = QM;
    Value *Addr = getQuadword(B, A, Quad);
    Q = B.CreateBitCast(Q, F4);
    Value *C = Mask == 15 ? B.CreateIntrinsic(Intrinsic::vu_sq, {}, {Q, Addr})
                          : B.CreateIntrinsic(Intrinsic::vu_sq_mask, {},
                                              {Q, Addr, B.getInt32(Mask)});
    copyAAMetadata(SI, C);
  }
  return true;
}

bool VULowerMemory::runOnFunction(Function &F) {
  DL = &F.getDataLayout();
  SmallVector<Instruction *, 32> Accesses;
  for (Instruction &I : instructions(F))
    if (isa<LoadInst, StoreInst>(I))
      Accesses.push_back(&I);
  for (Instruction *I : Accesses) {
    // A failed access gets a diagnostic; it is removed so that instruction
    // selection does not report it again.
    if (auto *LI = dyn_cast<LoadInst>(I)) {
      if (!lowerLoad(*LI))
        LI->replaceAllUsesWith(PoisonValue::get(LI->getType()));
    } else {
      lowerStore(*cast<StoreInst>(I));
    }
    // The byte address is not used any more; loop strength reduction would
    // make induction variables of it.
    Value *Ptr = getLoadStorePointerOperand(I);
    I->eraseFromParent();
    RecursivelyDeleteTriviallyDeadInstructions(Ptr);
  }
  return !Accesses.empty();
}
