//===- MipsR5900AlignVersion.cpp - R5900 loop versioning on alignment -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The 128-bit loads and stores of the R5900 (LQ and SQ for MMI, LQC2 and SQC2
// for VU0) need 16-byte aligned addresses; an unaligned vector access takes
// LDL/LDR or SDL/SDR pairs through the GPRs. A loop over arrays passed as
// pointers, such as
//
//   for (i = 0; i < n; i++)
//     y[i] = a * x[i] + y[i];
//
// does not tell the loop vectorizer that x and y are aligned, which they
// usually are, so it costs the vector accesses as unaligned and keeps the
// loop scalar. This pass, which runs before the vectorizers, versions such a
// loop on ((x | y) & 15) == 0. In the aligned copy, the arrays are
// llvm.ptrmask(x, -16) with an align 16 return attribute: the same pointers
// there, whose alignment the cost model (MipsTTIImpl::getMemoryOpCost) and
// InferAlignment see. The other copy is the original loop. An array that is
// a global of this module is aligned to 16 bytes instead (if its alignment
// may be raised), without a check.
//
// A loop is versioned when it is innermost, one block, has no calls and
// only inductions and reductions as header phis, costs less as vectors with
// its accesses aligned (not so with a multiply of 32-bit values), and each of
// its scalar loads and stores of a type that MMI or VU0 vectorizes is an
// element p[i] of a loop-invariant array p of unknown alignment (or of a
// 16-byte aligned one), with i an induction variable that starts at a
// multiple of 16 bytes and steps by one element, or an element p[i + c] of an
// unrolled loop whose i steps by a multiple of 16 bytes. Prefetches are
// allowed, other calls not.
//
//===----------------------------------------------------------------------===//

#include "MipsR5900AlignVersion.h"
#include "MipsSubtarget.h"
#include "MipsTargetMachine.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/IVDescriptors.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

using namespace llvm;
using namespace llvm::PatternMatch;

#define DEBUG_TYPE "mips-r5900-align-version"

STATISTIC(NumVersioned, "Number of loops versioned on alignment");
STATISTIC(NumGlobals, "Number of globals aligned to 16 bytes for a loop");

static cl::opt<bool>
    EnableAlignVersion("mips-r5900-align-version", cl::init(true), cl::Hidden,
                       cl::desc("Version R5900 loops over arrays of unknown "
                                "alignment on whether they are 16-byte "
                                "aligned"));

static cl::opt<unsigned> MaxLoopInsts(
    "mips-r5900-align-version-max-insts", cl::init(160), cl::Hidden,
    cl::desc("Largest loop (in instructions) that is versioned on alignment"));

namespace {

/// The array of an element access p[i]: the address of its element 0, or
/// null if the access is not such an element of a loop-invariant array.
Value *getArrayStart(Instruction *I, Loop *L, const DataLayout &DL) {
  Value *Ptr = getLoadStorePointerOperand(I);
  auto *GEP = dyn_cast<GetElementPtrInst>(Ptr);
  if (!GEP || !L->contains(GEP) || GEP->getNumIndices() == 0)
    return nullptr;
  // The last index is the induction variable, the others constants.
  for (unsigned K = 1; K < GEP->getNumOperands() - 1; ++K)
    if (!isa<ConstantInt>(GEP->getOperand(K)))
      return nullptr;
  gep_type_iterator Last = gep_type_begin(GEP);
  for (unsigned K = 1; K < GEP->getNumIndices(); ++K)
    ++Last;
  if (Last.isStruct())
    return nullptr;
  Type *EltTy = Last.getIndexedType();
  uint64_t EltSize = DL.getTypeAllocSize(EltTy);
  if (EltSize != DL.getTypeStoreSize(getLoadStoreType(I)) || 16 % EltSize)
    return nullptr;
  // The constant indices put element 0 at a multiple of 16 bytes from the
  // base, and the induction variable starts at a multiple of 16 bytes.
  APInt Offset(DL.getIndexTypeSizeInBits(GEP->getType()), 0);
  for (gep_type_iterator GTI = gep_type_begin(GEP); GTI != Last; ++GTI) {
    auto *C = cast<ConstantInt>(GTI.getOperand());
    if (StructType *STy = GTI.getStructTypeOrNull())
      Offset += DL.getStructLayout(STy)->getElementOffset(C->getZExtValue());
    else
      Offset += C->getValue().sextOrTrunc(Offset.getBitWidth()) *
                GTI.getSequentialElementStride(DL);
  }
  if (Offset.getZExtValue() % 16)
    return nullptr;
  // The index is i, or i + c for a constant c (as add or disjoint or) in an
  // unrolled loop, which then steps by a multiple of 16 bytes: the SLP
  // vectorizer makes vectors of the elements of an iteration.
  Value *Index = GEP->getOperand(GEP->getNumOperands() - 1);
  bool Unrolled = false;
  if (match(Index, m_AddLike(m_Value(), m_ConstantInt()))) {
    Index = cast<Instruction>(Index)->getOperand(0);
    Unrolled = true;
  }
  auto *Phi = dyn_cast<PHINode>(Index);
  if (!Phi || Phi->getParent() != L->getHeader() ||
      Phi->getNumIncomingValues() != 2)
    return nullptr;
  bool Start = false, Step = false;
  for (unsigned K = 0; K < 2; ++K) {
    Value *V = Phi->getIncomingValue(K);
    if (!L->contains(Phi->getIncomingBlock(K))) {
      auto *C = dyn_cast<ConstantInt>(V);
      Start = C && C->getZExtValue() * EltSize % 16 == 0;
      continue;
    }
    const APInt *C;
    if (match(V, m_Add(m_Specific(Phi), m_APInt(C))))
      Step = (C->isOne() && !Unrolled) ||
             (C->isStrictlyPositive() && C->getZExtValue() * EltSize % 16 == 0);
  }
  if (!Start || !Step)
    return nullptr;
  Value *Base = GEP->getPointerOperand();
  if (!L->isLoopInvariant(Base))
    return nullptr;
  return Base;
}

/// Whether a scalar load or store of \p Ty can become an MMI or VU0 vector
/// access.
bool isVectorizable(Type *Ty, const MipsSubtarget &ST) {
  if (Ty->isFloatTy())
    return ST.hasVU0();
  return Ty->isIntegerTy(8) || Ty->isIntegerTy(16) || Ty->isIntegerTy(32);
}

class AlignVersion {
  Function &F;
  const DataLayout &DL;
  const MipsSubtarget &ST;
  LoopInfo &LI;
  DominatorTree &DT;
  ScalarEvolution &SE;
  const TargetTransformInfo &TTI;

public:
  AlignVersion(Function &F, const MipsSubtarget &ST, LoopInfo &LI,
               DominatorTree &DT, ScalarEvolution &SE,
               const TargetTransformInfo &TTI)
      : F(F), DL(F.getDataLayout()), ST(ST), LI(LI), DT(DT), SE(SE), TTI(TTI) {}

  /// Whether the loop body, with its accesses aligned, costs less as 128-bit
  /// vectors than as VF scalar iterations, VF the elements of a vector of
  /// its widest loaded or stored type: otherwise, as with a multiply of
  /// 32-bit values, the loop vectorizer keeps the loop scalar and the copy is
  /// only code. A rough version of the loop vectorizer's cost model.
  bool isCheaperAsVectors(Loop *L) {
    constexpr auto Kind = TargetTransformInfo::TCK_RecipThroughput;
    unsigned Bits = 0;
    for (Instruction &I : *L->getHeader())
      if (isa<LoadInst, StoreInst>(&I))
        Bits = std::max<unsigned>(Bits,
                                  getLoadStoreType(&I)->getScalarSizeInBits());
    if (!Bits || Bits > 32)
      return false;
    unsigned VF = 128 / Bits;
    InstructionCost Vec = 0, Scalar = 0;
    for (Instruction &I : *L->getHeader()) {
      Type *Ty = isa<StoreInst>(&I) ? getLoadStoreType(&I) : I.getType();
      if (isa<LoadInst, StoreInst>(&I)) {
        unsigned AS = getLoadStoreAddressSpace(&I);
        Vec += TTI.getMemoryOpCost(I.getOpcode(), FixedVectorType::get(Ty, VF),
                                   Align(16), AS, Kind);
        Scalar += TTI.getMemoryOpCost(I.getOpcode(), Ty,
                                      getLoadStoreAlignment(&I), AS, Kind) *
                  VF;
        continue;
      }
      auto *BO = dyn_cast<BinaryOperator>(&I);
      if (!BO || (!Ty->isIntegerTy() && !Ty->isFloatTy()))
        continue;
      if (Ty->getScalarSizeInBits() > 32)
        return false;
      SmallVector<const Value *, 2> Ops(I.operand_values());
      auto Op0 = TTI.getOperandInfo(I.getOperand(0));
      auto Op1 = TTI.getOperandInfo(I.getOperand(1));
      Vec += TTI.getArithmeticInstrCost(
          I.getOpcode(), FixedVectorType::get(Ty, VF), Kind, Op0, Op1, Ops, &I);
      Scalar += TTI.getArithmeticInstrCost(I.getOpcode(), Ty, Kind, Op0, Op1,
                                           Ops, &I) *
                VF;
    }
    LLVM_DEBUG(dbgs() << "R5900 align version: " << L->getHeader()->getName()
                      << " costs " << Vec << " as vectors, " << Scalar << " as "
                      << VF << " scalar iterations\n");
    return Vec < Scalar;
  }

  /// The arrays of a loop that is versioned or whose globals are aligned:
  /// the arrays of unknown alignment to check, and the globals of this
  /// module to align instead (at a multiple of 16 bytes from their start).
  struct Arrays {
    SmallSetVector<Value *, 4> Check;
    SmallSetVector<GlobalVariable *, 4> Globals;
  };

  /// The arrays of \p L, or nothing if \p L is not a candidate.
  std::optional<Arrays> getArrays(Loop *L) {
    // One block: a loop with control flow in it is rarely vectorized.
    if (!L->isInnermost() || L->getNumBlocks() != 1 || !L->getUniqueExitBlock())
      return std::nullopt;
    Arrays A;
    unsigned NumInsts = 0;
    bool Vectorizable = false;
    for (Instruction &I : *L->getHeader()) {
      if (++NumInsts > MaxLoopInsts)
        return std::nullopt;
      if (auto *Call = dyn_cast<CallBase>(&I)) {
        // A prefetch only fetches a cache line: it does not stop the
        // vectorizers.
        auto *II = dyn_cast<IntrinsicInst>(Call);
        if (!II || (II->mayHaveSideEffects() &&
                    II->getIntrinsicID() != Intrinsic::prefetch))
          return std::nullopt;
        continue;
      }
      if (!isa<LoadInst, StoreInst>(&I) || I.isVolatile() || I.isAtomic())
        continue;
      Type *Ty = getLoadStoreType(&I);
      if (Ty->isVectorTy() || !isVectorizable(Ty, ST))
        continue;
      Value *Start = getArrayStart(&I, L, DL);
      if (!Start)
        return std::nullopt;
      Vectorizable = true;
      if (Start->getPointerAlignment(DL) >= Align(16))
        continue;
      APInt Offset(DL.getIndexTypeSizeInBits(Start->getType()), 0);
      auto *GV =
          dyn_cast<GlobalVariable>(Start->stripAndAccumulateConstantOffsets(
              DL, Offset, /*AllowNonInbounds=*/true));
      if (GV && Offset.getZExtValue() % 16 == 0 && GV->canIncreaseAlignment())
        A.Globals.insert(GV);
      else
        A.Check.insert(Start);
    }
    if (!Vectorizable || (A.Check.empty() && A.Globals.empty()))
      return std::nullopt;
    return A;
  }

  /// The loop vectorizer only takes header phis that are inductions or
  /// reductions: a loop that it cannot vectorize, such as a hash that
  /// multiplies its value each iteration, is not copied.
  bool hasVectorizablePhis(Loop *L) {
    for (PHINode &Phi : L->getHeader()->phis()) {
      InductionDescriptor ID;
      RecurrenceDescriptor RD;
      if (!InductionDescriptor::isInductionPHI(&Phi, L, &SE, ID) &&
          !RecurrenceDescriptor::isReductionPHI(&Phi, L, RD))
        return false;
    }
    return true;
  }

  void version(Loop *L, ArrayRef<Value *> Arrays) {
    BasicBlock *CheckBB = L->getLoopPreheader();
    BasicBlock *Exit = L->getUniqueExitBlock();
    // The aligned copy keeps L, behind a new preheader; the other copy is a
    // clone of it.
    BasicBlock *PH = SplitBlock(CheckBB, CheckBB->getTerminator(), &DT, &LI,
                                nullptr, L->getHeader()->getName() + ".ph");
    ValueToValueMapTy VMap;
    SmallVector<BasicBlock *, 8> CloneBlocks;
    Loop *Clone = cloneLoopWithPreheader(PH, CheckBB, L, VMap, ".unaligned",
                                         &LI, &DT, CloneBlocks);
    remapInstructionsInBlocks(CloneBlocks, VMap);

    // ((a | b | ...) & 15) == 0
    IRBuilder<> B(CheckBB->getTerminator());
    Type *IntPtrTy = DL.getIntPtrType(F.getContext());
    Value *Or = nullptr;
    for (Value *A : Arrays) {
      Value *Int = B.CreatePtrToInt(A, IntPtrTy);
      Or = Or ? B.CreateOr(Or, Int) : Int;
    }
    Value *Aligned =
        B.CreateICmpEQ(B.CreateAnd(Or, ConstantInt::get(IntPtrTy, 15)),
                       ConstantInt::get(IntPtrTy, 0), "aligned");
    B.CreateCondBr(Aligned, PH, Clone->getLoopPreheader());
    CheckBB->getTerminator()->eraseFromParent();
    DT.changeImmediateDominator(Exit, CheckBB);

    // The exit's phis take the values of the clone from its exiting blocks.
    SmallVector<BasicBlock *, 4> Exiting;
    L->getExitingBlocks(Exiting);
    for (PHINode &Phi : Exit->phis())
      for (BasicBlock *E : Exiting) {
        int K = Phi.getBasicBlockIndex(E);
        if (K < 0)
          continue;
        Value *V = Phi.getIncomingValue(K);
        Value *CloneV = VMap.lookup(V);
        Phi.addIncoming(CloneV ? CloneV : V, cast<BasicBlock>(VMap[E]));
      }

    // In the aligned copy, the arrays are those pointers with the alignment
    // known.
    B.SetInsertPoint(PH->getTerminator());
    for (Value *A : Arrays) {
      auto *Mask = cast<CallInst>(
          B.CreateIntrinsic(Intrinsic::ptrmask, {A->getType(), IntPtrTy},
                            {A, ConstantInt::getSigned(IntPtrTy, -16)}, nullptr,
                            A->getName() + ".aligned"));
      Mask->addRetAttr(Attribute::getWithAlignment(F.getContext(), Align(16)));
      A->replaceUsesWithIf(Mask, [&](Use &U) {
        auto *I = dyn_cast<Instruction>(U.getUser());
        return I && L->contains(I);
      });
    }
    SE.forgetLoop(L);
    ++NumVersioned;
    LLVM_DEBUG(dbgs() << "R5900 align version: " << L->getHeader()->getName()
                      << " on " << Arrays.size() << " arrays\n");
  }

  bool run() {
    bool Changed = false;
    SmallVector<Loop *, 8> Loops(LI.getLoopsInPreorder());
    for (Loop *L : Loops) {
      std::optional<Arrays> A = getArrays(L);
      if (!A)
        continue;
      // Loop-simplify form (a preheader, dedicated exits), as the loop
      // vectorizer makes it too.
      Changed |= simplifyLoop(L, &DT, &LI, &SE, nullptr, nullptr,
                              /*PreserveLCSSA=*/false);
      if (!L->isLoopSimplifyForm() || !L->getUniqueExitBlock() ||
          !hasVectorizablePhis(L) || !isCheaperAsVectors(L))
        continue;
      for (GlobalVariable *GV : A->Globals) {
        if (GV->getAlign().valueOrOne() < Align(16)) {
          GV->setAlignment(Align(16));
          ++NumGlobals;
          Changed = true;
        }
      }
      if (A->Check.empty())
        continue;
      // LCSSA, for the values of the loop used after it.
      formLCSSA(*L, DT, &LI, &SE);
      version(L, A->Check.getArrayRef());
      Changed = true;
    }
    return Changed;
  }
};

} // namespace

PreservedAnalyses MipsR5900AlignVersionPass::run(Function &F,
                                                 FunctionAnalysisManager &AM) {
  const MipsSubtarget &ST = TM.getSubtarget<MipsSubtarget>(F);
  if (!EnableAlignVersion || !ST.isR5900() || F.hasOptSize())
    return PreservedAnalyses::all();
  auto &LI = AM.getResult<LoopAnalysis>(F);
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
  auto &TTI = AM.getResult<TargetIRAnalysis>(F);
  if (!AlignVersion(F, ST, LI, DT, SE, TTI).run())
    return PreservedAnalyses::all();
  PreservedAnalyses PA;
  PA.preserve<LoopAnalysis>();
  PA.preserve<DominatorTreeAnalysis>();
  return PA;
}
