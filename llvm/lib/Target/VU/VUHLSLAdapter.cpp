//===-- VUHLSLAdapter.cpp - HLSL from clang's SPIR-V output to VU ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// HLSL for the VUs is compiled by clang for SPIR-V
// (-triple spirv-unknown-vulkan1.3-vertex or -compute) and then by `opt
// -mtriple=vu`. clang's output has the shape of a VU entry function already;
// this pass turns it into one (VUEntryABI.md) and gives the module the VU
// data layout:
//
//  * the entry ("hlsl.shader") becomes "vu-entry", vertex or compute;
//  * cbuffer members (address space 12, !hlsl.cbs) become uniforms at the
//    offsets of clang's cbuffer layout, the cbuffers one after the other;
//  * inputs (7) become vertex inputs in the order of their locations, their
//    UNPACK format from a suffix of the semantic (COLOR_V4_8U) or else from
//    the type (float3: V3_32), and for an integer input with more fields than
//    the format the value of the others (TEXCOORD_V2_16_F4096: int3 (s, t,
//    4096)); SV_VertexID is the vertex index;
//  * outputs (8) with a GIF_<register> semantic (GIF_XYZ2, or GIF_XYZF2_A
//    and GIF_XYZF2_B for two of one register) become the GIF registers, in
//    the order of their locations;
//  * the one structured buffer of a compute program becomes its items,
//    GetDimensions the item count and SV_DispatchThreadID the item index;
//  * static variables (10) that are written keep their value from item to
//    item (as in C), others are constants;
//  * the llvm.spv.* intrinsics become generic IR, llvm.matrix.multiply sums
//    of products, and the convergence tokens go away;
//  * calls to the functions that vu.hlsli declares without a body,
//    vu::__<name>, become the VU intrinsics llvm.vu.<name> (clip, fcand).
//
//===----------------------------------------------------------------------===//

#include "VU.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/VectorUtils.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IntrinsicsSPIRV.h"
#include "llvm/IR/IntrinsicsVU.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Regex.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

#define DEBUG_TYPE "vu-hlsl-adapter"

static cl::opt<unsigned>
    BufferQW("vu-buffer-qw",
             cl::desc("VU1 buffer size of HLSL vertex programs, quadwords"),
             cl::init(0));
static cl::opt<unsigned>
    SpillQW("vu-spill-qw",
            cl::desc("quadwords at the top of VU data memory for spills"),
            cl::init(0));
static cl::opt<unsigned>
    Indexed("vu-indexed",
            cl::desc("indexed HLSL vertex programs: vertices per batch"),
            cl::init(0));
static cl::opt<unsigned>
    Contexts("vu-contexts",
             cl::desc("contexts of HLSL vertex programs: 1 or 2 (default)"),
             cl::init(0));
static cl::opt<std::string>
    HeaderPrefix("vu-header-prefix",
                 cl::desc("prefix of the layout header's macros"),
                 cl::init("VU_"));
static cl::opt<std::string>
    CPU("vu-cpu", cl::desc("vu0 or vu1 for a compute program"), cl::init(""));

namespace {
/// SPIR-V address spaces of clang's HLSL output.
enum : unsigned {
  SPVInput = 7,
  SPVOutput = 8,
  SPVPrivate = 10,
  SPVDevice = 11,
  SPVConstant = 12,
};

/// SPIR-V decorations and built-ins in !spirv.Decorations.
enum : unsigned {
  DecBuiltIn = 11,
  DecLocation = 30,
  BuiltInPosition = 0,
  BuiltInVertexIndex = 42,
};

class Adapter {
  Module &M;
  LLVMContext &Ctx;
  Function *Entry = nullptr;
  bool Vertex = false;
  bool Failed = false;

  void error(const Twine &Msg) {
    if (Entry)
      Ctx.diagnose(DiagnosticInfoUnsupported(*Entry, "VU: " + Msg));
    else
      Ctx.emitError("VU: " + Msg);
    Failed = true;
  }

  void replaceGlobalBy(GlobalVariable *Old, GlobalVariable *New);
  void dropConvergence();
  void lowerCBuffers(const DataLayout &SrcDL);
  void lowerInputsOutputs();
  void lowerStatics();
  void lowerResources();
  void lowerIntrinsics();
  void lowerVUFunctions();

public:
  Adapter(Module &M) : M(M), Ctx(M.getContext()) {}
  bool run();
};
} // namespace

/// Uses of \p Old go to \p New, through an address space cast.
void Adapter::replaceGlobalBy(GlobalVariable *Old, GlobalVariable *New) {
  New->takeName(Old);
  Old->replaceAllUsesWith(ConstantExpr::getAddrSpaceCast(New, Old->getType()));
  Old->eraseFromParent();
}

/// Location or built-in of a global, from !spirv.Decorations.
static std::optional<unsigned> getDecoration(GlobalVariable *GV, unsigned D) {
  MDNode *MD = GV->getMetadata("spirv.Decorations");
  if (!MD)
    return std::nullopt;
  for (const MDOperand &Op : MD->operands()) {
    auto *N = dyn_cast<MDNode>(Op);
    if (!N || N->getNumOperands() < 2)
      continue;
    auto *K = mdconst::dyn_extract<ConstantInt>(N->getOperand(0));
    auto *V = mdconst::dyn_extract<ConstantInt>(N->getOperand(1));
    if (K && V && K->getZExtValue() == D)
      return V->getZExtValue();
  }
  return std::nullopt;
}

void Adapter::dropConvergence() {
  // The VU has one thread: the convergence tokens mean nothing.
  for (Function &F : M)
    for (Instruction &I : make_early_inc_range(instructions(F))) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      if (CB->getOperandBundle(LLVMContext::OB_convergencectrl)) {
        CallBase *New = CallBase::removeOperandBundle(
            CB, LLVMContext::OB_convergencectrl, CB->getIterator());
        CB->replaceAllUsesWith(New);
        CB->eraseFromParent();
      }
    }
  for (Function &F : M)
    for (Instruction &I : make_early_inc_range(instructions(F)))
      if (auto *II = dyn_cast<IntrinsicInst>(&I))
        switch (II->getIntrinsicID()) {
        case Intrinsic::experimental_convergence_entry:
        case Intrinsic::experimental_convergence_anchor:
        case Intrinsic::experimental_convergence_loop:
          II->replaceAllUsesWith(PoisonValue::get(II->getType()));
          II->eraseFromParent();
          break;
        default:
          break;
        }
  for (Function &F : M)
    F.removeFnAttr(Attribute::Convergent);
}

/// The layout struct of a cbuffer handle type
/// target("spirv.VulkanBuffer", %__cblayout_X, ...).
static StructType *getCBufferLayout(Type *T) {
  auto *TT = dyn_cast<TargetExtType>(T);
  if (!TT || TT->getNumTypeParameters() < 1)
    return nullptr;
  return dyn_cast<StructType>(TT->getTypeParameter(0));
}

void Adapter::lowerCBuffers(const DataLayout &SrcDL) {
  NamedMDNode *CBs = M.getNamedMetadata("hlsl.cbs");
  if (!CBs)
    return;
  uint64_t Base = 0;
  for (MDNode *CB : CBs->operands()) {
    if (!CB->getNumOperands())
      continue;
    auto *VM = dyn_cast_or_null<ValueAsMetadata>(CB->getOperand(0).get());
    auto *Handle = VM ? dyn_cast<GlobalVariable>(VM->getValue()) : nullptr;
    StructType *Layout =
        Handle ? getCBufferLayout(Handle->getValueType()) : nullptr;
    if (!Layout) {
      error("cbuffer without a layout");
      continue;
    }
    // The members are the elements of the layout that are not padding.
    uint64_t Offset = 0;
    unsigned Member = 1;
    for (Type *ET : Layout->elements()) {
      uint64_t Size;
      auto *TT = dyn_cast<TargetExtType>(ET);
      if (TT && TT->getName() == "spirv.Padding") {
        Offset += TT->getIntParameter(0);
        continue;
      }
      Size = SrcDL.getTypeAllocSize(ET);
      if (Member < CB->getNumOperands())
        if (auto *VM =
                dyn_cast_or_null<ValueAsMetadata>(CB->getOperand(Member).get()))
          if (auto *GV = dyn_cast<GlobalVariable>(VM->getValue())) {
            auto *New = new GlobalVariable(
                M, GV->getValueType(), /*isConstant=*/false,
                GlobalValue::ExternalLinkage, nullptr, "", nullptr,
                GlobalValue::NotThreadLocal, VUAS::Uniform);
            New->setMetadata(
                "vu.uniform",
                MDNode::get(Ctx, ConstantAsMetadata::get(ConstantInt::get(
                                     Type::getInt32Ty(Ctx), Base + Offset))));
            replaceGlobalBy(GV, New);
          }
      ++Member;
      Offset += Size;
    }
    // The next cbuffer starts on a quadword.
    Base += alignTo(Offset, 16);
    // The handle and its creation are not needed.
    for (User *U : make_early_inc_range(Handle->users()))
      if (auto *SI = dyn_cast<StoreInst>(U)) {
        Value *V = SI->getValueOperand();
        SI->eraseFromParent();
        RecursivelyDeleteTriviallyDeadInstructions(V);
      }
  }
  CBs->eraseFromParent();
}

/// The format suffix of a semantic: _V4_8U, and for an integer input with more
/// fields than the format, the value of the others (_V2_16_F4096).
static constexpr const char *FormatSuffix =
    "_(S|V2|V3|V4)_(32|16|8|5)(U?)(_F([0-9]+))?$";

/// The UNPACK format of an input: a suffix of the semantic (_V4_8U, then
/// _f<n> for _F<n>), or the 32-bit format of its type.
static std::string getFormat(StringRef Semantic, Type *T) {
  Regex Suffix(FormatSuffix, Regex::IgnoreCase);
  SmallVector<StringRef, 6> Matches;
  if (Suffix.match(Semantic, &Matches)) {
    std::string F = Matches[1].lower();
    F += F == "s" ? Matches[2].str() : "_" + Matches[2].str();
    F += Matches[3].lower();
    if (!Matches[5].empty())
      F += "_f" + Matches[5].str();
    return F;
  }
  unsigned N = 1;
  if (auto *VT = dyn_cast<FixedVectorType>(T))
    N = VT->getNumElements();
  return N == 1 ? "s32" : "v" + utostr(N) + "_32";
}

/// uitofp of values loaded from \p GV, also of their fields, is sitofp.
static void makeSignedConversions(GlobalVariable *GV) {
  SmallVector<Value *, 8> Work;
  for (User *U : GV->users())
    Work.push_back(U);
  while (!Work.empty()) {
    Value *V = Work.pop_back_val();
    if (isa<ConstantExpr, AddrSpaceCastInst, LoadInst, ShuffleVectorInst,
            ExtractElementInst>(V)) {
      for (User *U : V->users())
        Work.push_back(U);
      continue;
    }
    if (auto *UI = dyn_cast<UIToFPInst>(V)) {
      auto *SI = new SIToFPInst(UI->getOperand(0), UI->getType(), "",
                                UI->getIterator());
      SI->takeName(UI);
      SI->copyIRFlags(UI);
      UI->replaceAllUsesWith(SI);
      UI->eraseFromParent();
    }
  }
}

static bool hasFormatSuffix(StringRef Name) {
  return Regex(FormatSuffix, Regex::IgnoreCase).match(Name);
}

static bool isGifRegister(StringRef Name) {
  return is_contained(
      {"rgbaq", "st", "uv", "xyzf2", "xyz2", "fog", "xyzf3", "xyz3"},
      Name.lower());
}

void Adapter::lowerInputsOutputs() {
  for (GlobalVariable &GV : make_early_inc_range(M.globals())) {
    unsigned AS = GV.getAddressSpace();
    if (AS != SPVInput && AS != SPVOutput)
      continue;
    std::string Name = GV.getName().str();
    if (std::optional<unsigned> B = getDecoration(&GV, DecBuiltIn)) {
      if (AS == SPVInput && *B == BuiltInVertexIndex) {
        // SV_VertexID: the index of the vertex in the batch.
        for (User *U : make_early_inc_range(GV.users()))
          if (auto *LI = dyn_cast<LoadInst>(U)) {
            IRBuilder<> B(LI);
            Value *I =
                B.CreateIntrinsic(B.getInt16Ty(), Intrinsic::vu_item_index, {});
            LI->replaceAllUsesWith(B.CreateZExtOrTrunc(I, LI->getType()));
            LI->eraseFromParent();
          }
        if (GV.use_empty())
          GV.eraseFromParent();
        continue;
      }
      error(Twine(*B == BuiltInPosition
                      ? "SV_Position: a VU vertex program writes GIF registers "
                        "(GIF_XYZ2, GIF_XYZF2, ...)"
                      : "unsupported system value ") +
            (*B == BuiltInPosition ? "" : Name));
      continue;
    }
    std::optional<unsigned> Loc = getDecoration(&GV, DecLocation);
    if (!Loc) {
      error("input or output " + Name + " without a location");
      continue;
    }
    // clang names the global by the semantic and its index (the trailing
    // digits of the semantic are its index: GIF_XYZ2 is GIF_XYZ, 2), and adds
    // a suffix for a name used twice. The name is tried with and without the
    // digits.
    StringRef Full = StringRef(Name).split('.').first;
    StringRef Semantic = Full.rtrim("0123456789");
    auto *New = new GlobalVariable(M, GV.getValueType(), /*isConstant=*/false,
                                   GlobalValue::ExternalLinkage, nullptr, "",
                                   nullptr, GlobalValue::NotThreadLocal,
                                   AS == SPVInput ? VUAS::Input : VUAS::Output);
    std::string Kind;
    if (AS == SPVInput) {
      Kind = getFormat(Full, GV.getValueType());
      if (Kind.find('_') == std::string::npos || !hasFormatSuffix(Full))
        Kind = getFormat(Semantic, GV.getValueType());
      else
        Semantic = Full;
    } else {
      if (!Full.starts_with_insensitive("GIF_")) {
        error("output " + Full +
              ": a VU vertex program writes GIF registers "
              "(GIF_RGBAQ, GIF_ST, GIF_XYZ2, ...)");
        continue;
      }
      // GIF_<register>, and anything after another underscore to tell two
      // outputs to one register apart (GIF_XYZF2_A, GIF_XYZF2_B).
      StringRef Reg = Full.drop_front(4).split('_').first;
      Kind = isGifRegister(Reg) ? Reg.lower() : Reg.rtrim("0123456789").lower();
      if (!isGifRegister(Kind)) {
        error("output " + Full + ": unknown GIF register");
        continue;
      }
    }
    New->setMetadata(AS == SPVInput ? "vu.input" : "vu.output",
                     MDNode::get(Ctx, {ConstantAsMetadata::get(ConstantInt::get(
                                           Type::getInt32Ty(Ctx), *Loc)),
                                       MDString::get(Ctx, Kind)}));
    replaceGlobalBy(&GV, New);
    // The header names the inputs by their semantic, without the format.
    StringRef Base = AS == SPVInput ? Semantic : Full;
    if (hasFormatSuffix(Base)) {
      if (Regex("_F[0-9]+$", Regex::IgnoreCase).match(Base))
        Base = Base.substr(0, Base.rfind('_'));
      Base = Base.substr(0, Base.rfind('_', Base.rfind('_') - 1));
    }
    New->setName(Base.lower());
    // UNPACK makes 32-bit integers of 8- and 16-bit fields: an unsigned one
    // is never negative, and converts like a signed one (ITOF0).
    if (AS == SPVInput && !StringRef(Kind).split("_f").first.contains("32"))
      makeSignedConversions(New);
  }
}

void Adapter::lowerStatics() {
  for (GlobalVariable &GV : make_early_inc_range(M.globals())) {
    if (GV.getAddressSpace() != SPVPrivate || GV.getName().starts_with("llvm."))
      continue;
    bool Written = any_of(GV.users(), [&](User *U) {
      auto *SI = dyn_cast<StoreInst>(U);
      return SI && SI->getPointerOperand() == &GV;
    });
    if (!Written)
      continue;
    // A static that the program writes keeps its value from item to item.
    auto *New = new GlobalVariable(
        M, GV.getValueType(), /*isConstant=*/false,
        GlobalValue::InternalLinkage,
        GV.hasInitializer() ? GV.getInitializer() : nullptr, "", nullptr,
        GlobalValue::NotThreadLocal, VUAS::Private);
    New->setMetadata("vu.carry", MDNode::get(Ctx, {}));
    replaceGlobalBy(&GV, New);
  }
}

void Adapter::lowerResources() {
  GlobalVariable *Items = nullptr;
  Type *ItemsTy = nullptr;
  for (Function &F : M)
    for (Instruction &I : make_early_inc_range(instructions(F))) {
      auto *II = dyn_cast<IntrinsicInst>(&I);
      if (!II)
        continue;
      IRBuilder<> B(II);
      switch (II->getIntrinsicID()) {
      case Intrinsic::spv_resource_handlefrombinding:
      case Intrinsic::spv_resource_handlefromimplicitbinding: {
        auto *TT = dyn_cast<TargetExtType>(II->getType());
        if (!TT || TT->getNumTypeParameters() < 1) {
          error("unsupported resource");
          break;
        }
        Type *T = TT->getTypeParameter(0);
        // A cbuffer's handle (see lowerCBuffers).
        auto *ST = dyn_cast<StructType>(T);
        if (II->use_empty() ||
            (ST && ST->hasName() && ST->getName().starts_with("__cblayout"))) {
          II->replaceAllUsesWith(PoisonValue::get(II->getType()));
          II->eraseFromParent();
          break;
        }
        if (Items && T != ItemsTy) {
          error("a compute program has one structured buffer");
          break;
        }
        if (!Items) {
          ItemsTy = T;
          std::string Name = "items";
          if (auto *Str = dyn_cast<GlobalVariable>(
                  II->getArgOperand(II->arg_size() - 1)->stripPointerCasts()))
            if (auto *CDA =
                    dyn_cast_or_null<ConstantDataArray>(Str->getInitializer()))
              if (CDA->isCString())
                Name = CDA->getAsCString().str();
          Items = new GlobalVariable(M, T, false, GlobalValue::ExternalLinkage,
                                     nullptr, Name, nullptr,
                                     GlobalValue::NotThreadLocal, VUAS::Items);
        }
        // Users of the handle refer to the items.
        II->replaceAllUsesWith(PoisonValue::get(II->getType()));
        II->eraseFromParent();
        break;
      }
      default:
        break;
      }
    }
  for (Function &F : M)
    for (Instruction &I : make_early_inc_range(instructions(F))) {
      auto *II = dyn_cast<IntrinsicInst>(&I);
      if (!II)
        continue;
      IRBuilder<> B(II);
      Value *R = nullptr;
      switch (II->getIntrinsicID()) {
      case Intrinsic::spv_resource_getpointer:
        if (!Items) {
          error("a resource that is not the items");
          continue;
        }
        R = B.CreateAddrSpaceCast(
            B.CreateGEP(ItemsTy, Items, {B.getInt16(0), II->getArgOperand(1)}),
            II->getType());
        break;
      case Intrinsic::spv_resource_getdimensions_x:
        R = B.CreateZExtOrTrunc(
            B.CreateIntrinsic(B.getInt16Ty(), Intrinsic::vu_item_count, {}),
            II->getType());
        break;
      case Intrinsic::spv_thread_id: {
        // SV_DispatchThreadID: the item; the VU has one thread.
        auto *Dim = dyn_cast<ConstantInt>(II->getArgOperand(0));
        if (Dim && Dim->isZero())
          R = B.CreateZExtOrTrunc(
              B.CreateIntrinsic(B.getInt16Ty(), Intrinsic::vu_item_index, {}),
              II->getType());
        else
          R = ConstantInt::get(II->getType(), 0);
        break;
      }
      default:
        continue;
      }
      II->replaceAllUsesWith(R);
      II->eraseFromParent();
    }
}

static Value *getDot(IRBuilder<> &B, Value *X, Value *Y) {
  auto *VT = cast<FixedVectorType>(X->getType());
  Value *P = B.CreateFMul(X, Y);
  Value *S = B.CreateExtractElement(P, uint64_t(0));
  for (unsigned I = 1, E = VT->getNumElements(); I != E; ++I)
    S = B.CreateFAdd(S, B.CreateExtractElement(P, I));
  return S;
}

/// Column-major R x K times K x C: column j of the result is the sum over k
/// of column k of A times B[k][j].
static Value *getMatrixMultiply(IRBuilder<> &B, IntrinsicInst *II) {
  Value *A = II->getArgOperand(0), *Bm = II->getArgOperand(1);
  unsigned R = cast<ConstantInt>(II->getArgOperand(2))->getZExtValue();
  unsigned K = cast<ConstantInt>(II->getArgOperand(3))->getZExtValue();
  unsigned C = cast<ConstantInt>(II->getArgOperand(4))->getZExtValue();
  auto Column = [&](Value *M, unsigned Rows, unsigned J) -> Value * {
    if (Rows == 1)
      return B.CreateExtractElement(M, J);
    SmallVector<int, 4> Mask;
    for (unsigned I = 0; I != Rows; ++I)
      Mask.push_back(J * Rows + I);
    return B.CreateShuffleVector(M, Mask);
  };
  // The products are summed without reassociation, so a vertex transform
  // gives the same bits whatever the code around it: first the columns with
  // a constant factor, then the others in order. With w = 1 that is
  // m3 + m0 * x + m1 * y + m2 * z, which ACC computes as MULAw, MADDAx,
  // MADDAy, MADDz: the loaded vertex is three steps from the result.
  FastMathFlags SumFMF = B.getFastMathFlags();
  SumFMF.setAllowReassoc(false);
  Value *Res = PoisonValue::get(II->getType());
  for (unsigned J = 0; J != C; ++J) {
    SmallVector<unsigned, 4> Order;
    for (unsigned KI = 0; KI != K; ++KI)
      if (isa_and_nonnull<Constant>(findScalarElement(Bm, J * K + KI)))
        Order.push_back(KI);
    for (unsigned KI = 0; KI != K; ++KI)
      if (!is_contained(Order, KI))
        Order.push_back(KI);
    Value *Sum = nullptr;
    for (unsigned KI : Order) {
      Value *S = B.CreateExtractElement(Bm, J * K + KI);
      Value *ColK = Column(A, R, KI);
      if (R > 1)
        S = B.CreateVectorSplat(R, S);
      Value *P = B.CreateFMul(ColK, S);
      Sum = Sum ? B.CreateFAddFMF(Sum, P, SumFMF) : P;
    }
    for (unsigned I = 0; I != R; ++I)
      Res = B.CreateInsertElement(
          Res, R > 1 ? B.CreateExtractElement(Sum, I) : Sum, J * R + I);
  }
  return Res;
}

void Adapter::lowerIntrinsics() {
  for (Function &F : M)
    for (Instruction &I : make_early_inc_range(instructions(F))) {
      auto *II = dyn_cast<IntrinsicInst>(&I);
      if (!II)
        continue;
      IRBuilder<> B(II);
      B.setFastMathFlags(isa<FPMathOperator>(II) ? II->getFastMathFlags()
                                                 : FastMathFlags());
      Type *T = II->getType();
      auto Arg = [&](unsigned N) { return II->getArgOperand(N); };
      auto Splat = [&](double C) { return ConstantFP::get(T, C); };
      Value *R = nullptr;
      switch (II->getIntrinsicID()) {
      case Intrinsic::spv_fdot:
        R = getDot(B, Arg(0), Arg(1));
        break;
      case Intrinsic::spv_length:
        R = Arg(0)->getType()->isVectorTy()
                ? B.CreateUnaryIntrinsic(Intrinsic::sqrt,
                                         getDot(B, Arg(0), Arg(0)))
                : B.CreateUnaryIntrinsic(Intrinsic::fabs, Arg(0));
        break;
      case Intrinsic::spv_distance: {
        Value *D = B.CreateFSub(Arg(0), Arg(1));
        R = D->getType()->isVectorTy()
                ? B.CreateUnaryIntrinsic(Intrinsic::sqrt, getDot(B, D, D))
                : B.CreateUnaryIntrinsic(Intrinsic::fabs, D);
        break;
      }
      case Intrinsic::spv_normalize: {
        Value *X = Arg(0);
        if (!X->getType()->isVectorTy()) {
          R = Splat(1.0);
          break;
        }
        Value *L = B.CreateUnaryIntrinsic(Intrinsic::sqrt, getDot(B, X, X));
        Value *Inv = B.CreateFDiv(ConstantFP::get(L->getType(), 1.0), L);
        R = B.CreateFMul(
            X, B.CreateVectorSplat(cast<FixedVectorType>(T)->getNumElements(),
                                   Inv));
        break;
      }
      case Intrinsic::spv_frac:
        R = B.CreateFSub(Arg(0),
                         B.CreateUnaryIntrinsic(Intrinsic::floor, Arg(0)));
        break;
      case Intrinsic::spv_rsqrt:
        R = B.CreateFDiv(Splat(1.0),
                         B.CreateUnaryIntrinsic(Intrinsic::sqrt, Arg(0)));
        break;
      case Intrinsic::spv_saturate:
        R = B.CreateBinaryIntrinsic(
            Intrinsic::minnum,
            B.CreateBinaryIntrinsic(Intrinsic::maxnum, Arg(0), Splat(0.0)),
            Splat(1.0));
        break;
      case Intrinsic::spv_nclamp:
        R = B.CreateBinaryIntrinsic(
            Intrinsic::minnum,
            B.CreateBinaryIntrinsic(Intrinsic::maxnum, Arg(0), Arg(1)), Arg(2));
        break;
      case Intrinsic::spv_smoothstep: {
        // t = saturate((x - a) / (b - a)); t * t * (3 - 2 t)
        Value *TT = B.CreateFDiv(B.CreateFSub(Arg(2), Arg(0)),
                                 B.CreateFSub(Arg(1), Arg(0)));
        TT = B.CreateBinaryIntrinsic(
            Intrinsic::minnum,
            B.CreateBinaryIntrinsic(Intrinsic::maxnum, TT, Splat(0.0)),
            Splat(1.0));
        R = B.CreateFMul(
            B.CreateFMul(TT, TT),
            B.CreateFSub(Splat(3.0), B.CreateFMul(Splat(2.0), TT)));
        break;
      }
      case Intrinsic::spv_sign: {
        // An integer: (x > 0) - (x < 0)
        Value *X = Arg(0);
        Type *FT = X->getType();
        Value *Zero = ConstantFP::get(FT, 0.0);
        Value *P = B.CreateSelect(B.CreateFCmpOGT(X, Zero),
                                  ConstantFP::get(FT, 1.0), Zero);
        Value *N = B.CreateSelect(B.CreateFCmpOLT(X, Zero),
                                  ConstantFP::get(FT, 1.0), Zero);
        R = B.CreateFPToSI(B.CreateFSub(P, N), T);
        break;
      }
      case Intrinsic::spv_lerp:
        R = B.CreateFAdd(Arg(0),
                         B.CreateFMul(B.CreateFSub(Arg(1), Arg(0)), Arg(2)));
        break;
      case Intrinsic::spv_degrees:
        R = B.CreateFMul(Arg(0), Splat(180.0 / numbers::pi));
        break;
      case Intrinsic::spv_radians:
        R = B.CreateFMul(Arg(0), Splat(numbers::pi / 180.0));
        break;
      case Intrinsic::spv_any:
      case Intrinsic::spv_all: {
        // Of the fields of a vector of booleans (or of numbers: != 0).
        Value *X = Arg(0);
        bool Any = II->getIntrinsicID() == Intrinsic::spv_any;
        auto Bool = [&](Value *E) -> Value * {
          if (E->getType()->isIntegerTy(1))
            return E;
          if (E->getType()->isFloatingPointTy())
            return B.CreateFCmpUNE(E, ConstantFP::get(E->getType(), 0.0));
          return B.CreateICmpNE(E, ConstantInt::get(E->getType(), 0));
        };
        auto *VT = dyn_cast<FixedVectorType>(X->getType());
        if (!VT) {
          R = Bool(X);
        } else {
          // A comparison of vectors becomes comparisons of their fields.
          auto *Cmp = dyn_cast<CmpInst>(X);
          for (unsigned K = 0, E = VT->getNumElements(); K != E; ++K) {
            Value *Elt;
            if (Cmp)
              Elt = B.CreateCmp(Cmp->getPredicate(),
                                B.CreateExtractElement(Cmp->getOperand(0), K),
                                B.CreateExtractElement(Cmp->getOperand(1), K));
            else
              Elt = Bool(B.CreateExtractElement(X, K));
            R = !R ? Elt : Any ? B.CreateOr(R, Elt) : B.CreateAnd(R, Elt);
          }
        }
        if (R->getType() != T)
          R = B.CreateZExt(R, T);
        break;
      }
      case Intrinsic::matrix_multiply:
        R = getMatrixMultiply(B, II);
        break;
      default:
        if (II->getIntrinsicID() != Intrinsic::not_intrinsic &&
            II->getCalledFunction()->getName().starts_with("llvm.spv.")) {
          error("unsupported HLSL intrinsic " +
                II->getCalledFunction()->getName());
          II->replaceAllUsesWith(PoisonValue::get(T));
          II->eraseFromParent();
        }
        continue;
      }
      II->replaceAllUsesWith(R);
      II->eraseFromParent();
    }
}

/// The name of the VU function \p F: <name> of vu::__<name> (mangled
/// _ZN2vu<length>__<name>...), or empty.
static StringRef getVUFunction(const Function &F) {
  StringRef N = F.getName();
  if (!F.isDeclaration() || !N.consume_front("_ZN2vu"))
    return "";
  unsigned Len;
  if (N.consumeInteger(10, Len) || N.size() < Len)
    return "";
  N = N.take_front(Len);
  return N.consume_front("__") ? N : "";
}

void Adapter::lowerVUFunctions() {
  if (none_of(M, [](const Function &F) {
        return !getVUFunction(F).empty() && !F.use_empty();
      }))
    return;
  // vu.hlsli's functions around them are inlined into the entry first, where
  // their operands are known (a constant mask). The copies in vu.hlsli's own
  // functions are left; those are not used, and go away.
  for (bool Changed = true; Changed;) {
    Changed = false;
    SmallVector<CallBase *, 8> Calls;
    for (Instruction &I : instructions(*Entry))
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (Function *Callee = CB->getCalledFunction();
            Callee && !Callee->isDeclaration())
          Calls.push_back(CB);
    for (CallBase *CB : Calls) {
      InlineFunctionInfo IFI;
      Changed |= InlineFunction(*CB, IFI).isSuccess();
    }
  }
  for (Function &F : make_early_inc_range(M)) {
    StringRef Name = getVUFunction(F);
    if (Name.empty())
      continue;
    for (User *U : make_early_inc_range(F.users())) {
      auto *CI = dyn_cast<CallInst>(U);
      if (!CI || CI->getCalledFunction() != &F || CI->getFunction() != Entry)
        continue;
      IRBuilder<> B(CI);
      Type *F4 = FixedVectorType::get(B.getFloatTy(), 4);
      // A float vector of 1 to 4 fields as the fields of a VU vector.
      auto Vec4 = [&](Value *V) -> Value * {
        auto *VT = dyn_cast<FixedVectorType>(V->getType());
        if (!VT)
          return B.CreateInsertElement(PoisonValue::get(F4), V, uint64_t(0));
        SmallVector<int, 4> Mask;
        for (unsigned K = 0; K != 4; ++K)
          Mask.push_back(K < VT->getNumElements() ? int(K) : PoisonMaskElem);
        return B.CreateShuffleVector(V, Mask);
      };
      Value *R = nullptr;
      if (Name == "clip" && CI->arg_size() == 2) {
        // clip(float3 v, float w): w is read from field w.
        B.CreateIntrinsic(
            Intrinsic::vu_clip, {},
            {Vec4(CI->getArgOperand(0)),
             B.CreateInsertElement(PoisonValue::get(F4), CI->getArgOperand(1),
                                   uint64_t(3))});
      } else if (Name == "fcand" && CI->arg_size() == 1) {
        auto *Mask = dyn_cast<ConstantInt>(CI->getArgOperand(0));
        if (!Mask || !isUInt<24>(Mask->getZExtValue())) {
          error("vu::clipped: the mask is a 24-bit constant");
          R = PoisonValue::get(CI->getType());
        } else {
          R = B.CreateZExt(
              B.CreateIntrinsic(B.getInt16Ty(), Intrinsic::vu_fcand,
                                {B.getInt32(Mask->getZExtValue())}),
              CI->getType());
        }
      } else {
        error("unknown VU function vu::__" + Name);
        R = CI->getType()->isVoidTy() ? nullptr
                                      : PoisonValue::get(CI->getType());
      }
      if (R)
        CI->replaceAllUsesWith(R);
      CI->eraseFromParent();
    }
    if (F.use_empty())
      F.eraseFromParent();
  }
}

bool Adapter::run() {
  for (Function &F : M) {
    Attribute S = F.getFnAttribute("hlsl.shader");
    if (!S.isValid() || F.isDeclaration())
      continue;
    if (Entry) {
      error("one entry point per module");
      return true;
    }
    Entry = &F;
    StringRef Stage = S.getValueAsString();
    if (Stage != "vertex" && Stage != "compute") {
      error("a VU program is a vertex or compute shader, not " + Stage);
      return true;
    }
    Vertex = Stage == "vertex";
  }
  if (!Entry)
    return false;

  // Layouts are computed with clang's data layout, then the module becomes a
  // VU module.
  DataLayout SrcDL = M.getDataLayout();
  M.setTargetTriple(Triple("vu"));
  M.setDataLayout(Triple("vu").computeDataLayout());

  dropConvergence();
  lowerCBuffers(SrcDL);
  lowerInputsOutputs();
  lowerStatics();
  lowerResources();
  lowerIntrinsics();
  lowerVUFunctions();

  Entry->removeFnAttr("hlsl.shader");
  Entry->removeFnAttr("hlsl.numthreads");
  Entry->addFnAttr("vu-entry", Vertex ? "vertex" : "compute");
  Entry->addFnAttr("vu-header-prefix", HeaderPrefix);
  if (BufferQW)
    Entry->addFnAttr("vu-buffer-qw", utostr(BufferQW));
  if (Indexed && Vertex)
    Entry->addFnAttr("vu-indexed", utostr(Indexed));
  if (Contexts && Vertex)
    Entry->addFnAttr("vu-contexts", utostr(Contexts));
  if (SpillQW)
    for (Function &F : M)
      if (!F.isDeclaration())
        F.addFnAttr("vu-spill-qw", utostr(SpillQW));
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    // The other functions are inlined into the entry (VUEntryLowering).
    if (&F != Entry)
      F.setLinkage(GlobalValue::InternalLinkage);
    F.removeFnAttr("target-cpu");
    F.removeFnAttr("target-features");
    if (!CPU.empty())
      F.addFnAttr("target-cpu", CPU);
  }
  // clang's helpers (handles, strings) that are left.
  for (GlobalVariable &GV : make_early_inc_range(M.globals())) {
    GV.removeDeadConstantUsers();
    if (GV.use_empty() && !GV.getName().starts_with("llvm.") &&
        (GV.hasLocalLinkage() || GV.isDeclaration()))
      GV.eraseFromParent();
  }
  if (GlobalVariable *Used = M.getGlobalVariable("llvm.compiler.used"))
    Used->eraseFromParent();
  return true;
}

namespace {
class VUHLSLAdapterLegacy : public ModulePass {
public:
  static char ID;
  VUHLSLAdapterLegacy() : ModulePass(ID) {}
  StringRef getPassName() const override { return "VU HLSL adapter"; }
  bool runOnModule(Module &M) override { return Adapter(M).run(); }
};
} // namespace

char VUHLSLAdapterLegacy::ID = 0;

INITIALIZE_PASS(VUHLSLAdapterLegacy, DEBUG_TYPE, "VU HLSL adapter", false,
                false)

ModulePass *llvm::createVUHLSLAdapterPass() {
  return new VUHLSLAdapterLegacy();
}

PreservedAnalyses VUHLSLAdapterPass::run(Module &M, ModuleAnalysisManager &) {
  return Adapter(M).run() ? PreservedAnalyses::none()
                          : PreservedAnalyses::all();
}
