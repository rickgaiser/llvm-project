//===-- VUEntryLowering.cpp - Micro programs from entry functions ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A frontend describes a micro program as an entry function and globals; this
// pass lays out VU data memory, builds the loop over the items of a batch and
// writes a C header with the layout for the EE (see VUEntryABI.md):
//
//   define void @k() "vu-entry"="vertex" | "compute"
//
// with the code for one item (vertex), or for the whole program (compute
// without llvm.vu.item.index). The globals it uses, by address space:
//
//   1 uniform  !vu.uniform !{i32 byte offset}     in the context, or after the
//                                                 count
//   2 input    !vu.input !{i32 order, !"v4_8u"}   one quadword per input
//   3 output   !vu.output !{i32 order, !"xyz2"}   one GIF register per output
//   4 items    [0 x T]                            the batch of a kernel
//   5 private  per item; !vu.carry keeps the value from item to item
//
// A vertex program (VU1) gets the layout of a double buffer at XTOP:
//   [counts][inputs of each vertex][GIF tag][outputs of each vertex]
// and above both buffers one or two contexts ("vu-contexts"), which the EE
// writes once per draw: [GIF tag with NLOOP 0][uniforms]. The counts name the
// context; the program copies its GIF tag in front of the outputs with NLOOP
// = the vertex count and sends them with XGKICK. An indexed vertex program
// ("vu-indexed"="<vertices>") transforms each vertex once into a table and
// then gathers the packet by an index list, NLOOP = the index count. Each
// buffer has [counts][inputs][indices]; above both:
//   [GIF tag][outputs][empty GIF tag][contexts][table][spills]
// A compute program gets, from quadword 0:
//   [count][uniforms][items]
//
//===----------------------------------------------------------------------===//

#include "VU.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IntrinsicsVU.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/ReplaceConstant.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"
#include <numeric>

using namespace llvm;

#define DEBUG_TYPE "vu-entry-lowering"

namespace {
/// UNPACK formats of vertex inputs: vn << 2 | vl, and bytes per vertex.
struct Format {
  StringLiteral Name;
  unsigned Code, Bytes;
};
constexpr Format Formats[] = {
    {"s32", 0x0, 4},    {"s16", 0x1, 2},   {"s8", 0x2, 1},
    {"v2_32", 0x4, 8},  {"v2_16", 0x5, 4}, {"v2_8", 0x6, 2},
    {"v3_32", 0x8, 12}, {"v3_16", 0x9, 6}, {"v3_8", 0xA, 3},
    {"v4_32", 0xC, 16}, {"v4_16", 0xD, 8}, {"v4_8", 0xE, 4},
    {"v4_5", 0xF, 2}};

/// GIF registers that a vertex program writes (PACKED mode).
struct GifReg {
  StringLiteral Name;
  unsigned Code;
};
constexpr GifReg GifRegs[] = {{"rgbaq", 0x1}, {"st", 0x2},   {"uv", 0x3},
                              {"xyzf2", 0x4}, {"xyz2", 0x5}, {"fog", 0xA},
                              {"xyzf3", 0xC}, {"xyz3", 0xD}};

/// A global of the program and its place.
struct Slot {
  GlobalVariable *GV;
  unsigned Order = 0;
  std::string Kind; // format or GIF register
  unsigned Code = 0;
  bool Unsigned = false;
  unsigned Bytes = 0;
  unsigned Fill = 0;   // inputs: fields (bit 0 = x) that the VIF sets to 1.0
  unsigned RowFill = 0; // inputs: fields that the VIF sets from ROW
  uint64_t Offset = 0; // uniforms: bytes from the start of the block
};

class EntryLowering {
  Module &M;
  Function &F;
  LLVMContext &Ctx;
  const DataLayout &DL;
  bool Vertex;
  SmallVector<Slot, 8> Uniforms, Inputs, Outputs;
  uint32_t Row[4] = {0, 0, 0, 0}; // the ROW values of RowSet's fields
  unsigned RowSet = 0;
  GlobalVariable *Items = nullptr;
  SmallVector<GlobalVariable *, 4> Privates;
  std::string Up, Prefix;
  bool Failed = false;

  void error(const Twine &Msg) {
    Ctx.diagnose(DiagnosticInfoUnsupported(F, "VU: " + Msg));
    Failed = true;
  }
  void inlineCalls();
  void collectGlobals();
  bool usedInF(GlobalVariable &GV);
  Value *qwPointer(IRBuilder<> &B, GlobalVariable *GV, Value *QW,
                   uint64_t ByteOffset = 0);
  void replaceGlobal(GlobalVariable *GV, Value *Ptr);
  void dropIgnoredFields(const Slot &S);
  void hoistToEntry(BasicBlock *Entry);
  void addAliasScopes(ArrayRef<Value *> In, ArrayRef<Value *> Out);
  void buildGather(IRBuilder<> &B, Value *Top, Value *Out, Value *NQ,
                   uint64_t TabAt, uint64_t IdxAt, unsigned NOut);

public:
  EntryLowering(Module &M, Function &F, bool Vertex)
      : M(M), F(F), Ctx(M.getContext()), DL(M.getDataLayout()), Vertex(Vertex) {
  }
  bool run(std::string &Header);
  bool failed() const { return Failed; }
};
} // namespace

void EntryLowering::inlineCalls() {
  // Everything is inlined: the VU has no calls.
  for (bool Changed = true; Changed;) {
    Changed = false;
    SmallVector<CallBase *, 8> Calls;
    for (Instruction &I : instructions(F))
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (Function *Callee = CB->getCalledFunction();
            Callee && !Callee->isDeclaration())
          Calls.push_back(CB);
    for (CallBase *CB : Calls) {
      InlineFunctionInfo IFI;
      if (InlineFunction(*CB, IFI).isSuccess())
        Changed = true;
    }
  }
}

bool EntryLowering::usedInF(GlobalVariable &GV) {
  for (User *U : GV.users())
    if (auto *I = dyn_cast<Instruction>(U); I && I->getFunction() == &F)
      return true;
  return false;
}

static std::optional<uint64_t> getMDInt(MDNode *N, unsigned I) {
  if (!N || N->getNumOperands() <= I)
    return std::nullopt;
  if (auto *C = mdconst::dyn_extract<ConstantInt>(N->getOperand(I)))
    return C->getZExtValue();
  return std::nullopt;
}

static StringRef getMDString(MDNode *N, unsigned I) {
  if (!N || N->getNumOperands() <= I)
    return "";
  if (auto *S = dyn_cast<MDString>(N->getOperand(I)))
    return S->getString();
  return "";
}

void EntryLowering::collectGlobals() {
  SmallVector<Constant *, 8> Used;
  for (GlobalVariable &GV : M.globals())
    if (GV.getAddressSpace() >= VUAS::Uniform &&
        GV.getAddressSpace() <= VUAS::Private)
      Used.push_back(&GV);
  // Constant expressions on the globals become instructions in F, so that
  // each use can take the address that this pass computes.
  convertUsersOfConstantsToInstructions(Used, &F);

  for (GlobalVariable &GV : M.globals()) {
    unsigned AS = GV.getAddressSpace();
    if (AS < VUAS::Uniform || AS > VUAS::Private || !usedInF(GV))
      continue;
    Slot S{&GV};
    std::string Name = GV.getName().str();
    switch (AS) {
    case VUAS::Uniform: {
      std::optional<uint64_t> Off = getMDInt(GV.getMetadata("vu.uniform"), 0);
      if (!Off) {
        error("uniform " + Name + " has no !vu.uniform offset");
        continue;
      }
      S.Offset = *Off;
      S.Bytes = DL.getTypeAllocSize(GV.getValueType());
      Uniforms.push_back(S);
      break;
    }
    case VUAS::Input:
    case VUAS::Output: {
      MDNode *MD = GV.getMetadata(AS == VUAS::Input ? "vu.input" : "vu.output");
      std::optional<uint64_t> Order = getMDInt(MD, 0);
      std::string Kind = getMDString(MD, 1).lower();
      if (!Order || Kind.empty()) {
        error(Twine(AS == VUAS::Input ? "input " : "output ") + Name +
              " needs its order and " +
              (AS == VUAS::Input ? "format" : "GIF register"));
        continue;
      }
      S.Order = *Order;
      S.Kind = Kind;
      if (AS == VUAS::Input) {
        StringRef K = Kind;
        // _f<n>: the fields that the format does not give are n.
        std::optional<uint32_t> FillValue;
        if (size_t F = K.find("_f"); F != StringRef::npos) {
          uint32_t V;
          if (K.substr(F + 2).getAsInteger(10, V)) {
            error("input " + Name + ": bad field value in " + Kind);
            continue;
          }
          FillValue = V;
          K = K.substr(0, F);
        }
        if (K.size() > 2 && K.back() == 'u' && K != "v4_5")
          S.Unsigned = true, K = K.drop_back();
        auto It =
            find_if(Formats, [&](const Format &Fm) { return Fm.Name == K; });
        if (It == std::end(Formats)) {
          error("input " + Name + ": unknown UNPACK format " + Kind);
          continue;
        }
        S.Code = It->Code;
        S.Bytes = It->Bytes;
        // A float vector input with more fields than a 32-bit format gives:
        // the VIF sets the others to 1.0 (STMASK, COL), float3 st with
        // V2_32 is (s, t, 1). The program loads them as the others.
        auto *VT = dyn_cast<FixedVectorType>(GV.getValueType());
        unsigned VN = (S.Code >> 2) + 1;
        if (VT && VT->getElementType()->isFloatTy() && (S.Code & 3) == 0 &&
            K != "s32" && VT->getNumElements() > VN)
          for (unsigned L = VN; L != VT->getNumElements(); ++L)
            S.Fill |= 1 << L;
        // An integer vector input with more fields than the format and a
        // value for them: the VIF sets them from ROW, which the EE sets to
        // the values of the header's ROW. A field has one value for all
        // inputs.
        if (FillValue) {
          if (!VT || !VT->getElementType()->isIntegerTy() ||
              VT->getNumElements() <= VN || K.starts_with("s")) {
            error("input " + Name +
                  ": a field value needs an integer vector with more "
                  "fields than its format " +
                  K);
            continue;
          }
          for (unsigned L = VN; L != VT->getNumElements(); ++L) {
            if ((RowSet & (1 << L)) && Row[L] != *FillValue) {
              error("input " + Name + ": field " + Twine("xyzw"[L]) +
                    " has another value in another input");
              break;
            }
            Row[L] = *FillValue;
            RowSet |= 1 << L;
            S.RowFill |= 1 << L;
          }
        }
        Inputs.push_back(S);
      } else {
        auto It =
            find_if(GifRegs, [&](const GifReg &R) { return R.Name == Kind; });
        if (It == std::end(GifRegs)) {
          error("output " + Name + ": unknown GIF register " + Kind);
          continue;
        }
        S.Code = It->Code;
        Outputs.push_back(S);
      }
      break;
    }
    case VUAS::Items:
      if (Items)
        error("a compute program has one array of items");
      Items = &GV;
      break;
    case VUAS::Private:
      Privates.push_back(&GV);
      break;
    }
  }
  auto ByOrder = [](const Slot &A, const Slot &B) { return A.Order < B.Order; };
  llvm::sort(Inputs, ByOrder);
  llvm::sort(Outputs, ByOrder);
  llvm::sort(Uniforms,
             [](const Slot &A, const Slot &B) { return A.Offset < B.Offset; });
}

/// The byte address of quadword \p QW plus \p ByteOffset, as a pointer in
/// the address space of \p GV. The scale of 16 is taken off again by
/// VULowerMemory.
Value *EntryLowering::qwPointer(IRBuilder<> &B, GlobalVariable *GV, Value *QW,
                                uint64_t ByteOffset) {
  Value *Bytes = B.CreateShl(QW, 4);
  if (ByteOffset)
    Bytes = B.CreateAdd(Bytes, B.getInt16(ByteOffset));
  return B.CreateIntToPtr(Bytes, GV->getType());
}

/// The loads and stores through \p Old (also through casts and address
/// arithmetic) go through \p New, which may be in another address space.
static void retargetPointer(Value *Old, Value *New) {
  for (Use &U : make_early_inc_range(Old->uses())) {
    User *Usr = U.getUser();
    if (isa<LoadInst>(Usr) || (isa<StoreInst>(Usr) && U.getOperandNo() == 1)) {
      U.set(New);
    } else if (auto *C = dyn_cast<AddrSpaceCastInst>(Usr)) {
      retargetPointer(C, New);
      if (C->use_empty())
        C->eraseFromParent();
    } else if (auto *G = dyn_cast<GetElementPtrInst>(Usr)) {
      IRBuilder<> B(G);
      SmallVector<Value *, 4> Idx(G->indices());
      Value *NG = B.CreateGEP(G->getSourceElementType(), New, Idx);
      retargetPointer(G, NG);
      if (G->use_empty())
        G->eraseFromParent();
    }
  }
}

void EntryLowering::replaceGlobal(GlobalVariable *GV, Value *Ptr) {
  for (Use &U : make_early_inc_range(GV->uses()))
    if (auto *I = dyn_cast<Instruction>(U.getUser());
        I && I->getFunction() == &F)
      U.set(Ptr);
}

/// The fields of a GIF register that PACKED mode does not read are undefined
/// in the stores to it, so nothing computes them: w of ST, z and w of UV, x,
/// y and z of FOG. Float vectors only: InstCombine may move the fields of an
/// integer vector into others, which the VU has no exact instruction for.
void EntryLowering::dropIgnoredFields(const Slot &S) {
  unsigned Ignored = StringSwitch<unsigned>(S.Kind)
                         .Case("st", 0x8)
                         .Case("uv", 0xC)
                         .Case("fog", 0x7)
                         .Default(0);
  if (!Ignored)
    return;
  for (Instruction &I : instructions(F)) {
    auto *SI = dyn_cast<StoreInst>(&I);
    auto *VT = SI ? dyn_cast<FixedVectorType>(SI->getValueOperand()->getType())
                  : nullptr;
    if (!VT || VT->getNumElements() != 4 ||
        !VT->getElementType()->isFloatTy() ||
        SI->getPointerOperand()->stripPointerCasts() != S.GV)
      continue;
    SmallVector<int, 4> Mask;
    for (int L = 0; L != 4; ++L)
      Mask.push_back(Ignored & (1 << L) ? PoisonMaskElem : L);
    IRBuilder<> B(SI);
    SI->setOperand(0, B.CreateShuffleVector(SI->getValueOperand(), Mask));
  }
}

/// Moves the loads from uniforms, and the address arithmetic for them, out of
/// the loop: uniforms are not written by the program.
void EntryLowering::hoistToEntry(BasicBlock *Entry) {
  SmallPtrSet<Value *, 16> Invariant;
  for (Instruction &I : *Entry)
    Invariant.insert(&I);
  std::function<bool(Value *)> Hoist = [&](Value *V) -> bool {
    if (isa<Constant>(V) || isa<Argument>(V) || Invariant.count(V))
      return true;
    auto *I = dyn_cast<Instruction>(V);
    if (!I || isa<PHINode>(I) || I->mayHaveSideEffects() ||
        (I->mayReadFromMemory() && !isa<LoadInst>(I)))
      return false;
    for (Value *Op : I->operands())
      if (!Hoist(Op))
        return false;
    I->moveBefore(Entry->getTerminator()->getIterator());
    Invariant.insert(I);
    return true;
  };
  SmallVector<LoadInst *, 16> Loads;
  for (BasicBlock &BB : F)
    if (&BB != Entry)
      for (Instruction &I : BB)
        if (auto *LI = dyn_cast<LoadInst>(&I);
            LI && LI->hasMetadata("vu.uniform.load"))
          Loads.push_back(LI);
  for (LoadInst *LI : Loads)
    Hoist(LI);
  for (LoadInst *LI : Loads)
    LI->setMetadata("vu.uniform.load", nullptr);
}

/// Every load and store through \p Ptr, also through address arithmetic on
/// it.
static void forEachAccess(Value *Ptr, function_ref<void(Instruction *)> Fn) {
  SmallVector<Value *, 8> Work{Ptr};
  SmallPtrSet<Value *, 16> Seen;
  while (!Work.empty()) {
    Value *V = Work.pop_back_val();
    if (!Seen.insert(V).second)
      continue;
    for (User *U : V->users()) {
      if (isa<LoadInst, StoreInst>(U))
        Fn(cast<Instruction>(U));
      else if (isa<GetElementPtrInst, BitCastInst, AddrSpaceCastInst>(U))
        Work.push_back(U);
    }
  }
}

/// The inputs and uniforms are not written, and the outputs not read: alias
/// scopes tell the pipeliner that the loads of the next vertex can pass the
/// stores of this one. The items of a compute program are \p Out, read and
/// written: only the loads of the uniforms pass their stores.
void EntryLowering::addAliasScopes(ArrayRef<Value *> In,
                                   ArrayRef<Value *> Out) {
  MDBuilder MDB(Ctx);
  MDNode *Domain = MDB.createAnonymousAliasScopeDomain(F.getName());
  MDNode *InScope = MDB.createAnonymousAliasScope(Domain, "in");
  MDNode *OutScope = MDB.createAnonymousAliasScope(Domain, "out");
  MDNode *InList = MDNode::get(Ctx, {InScope});
  MDNode *OutList = MDNode::get(Ctx, {OutScope});
  auto Tag = [&](Instruction *I, MDNode *Scope, MDNode *NoAlias) {
    I->setMetadata(LLVMContext::MD_alias_scope,
                   MDNode::concatenate(
                       I->getMetadata(LLVMContext::MD_alias_scope), Scope));
    I->setMetadata(
        LLVMContext::MD_noalias,
        MDNode::concatenate(I->getMetadata(LLVMContext::MD_noalias), NoAlias));
  };
  for (Value *P : In)
    forEachAccess(P, [&](Instruction *I) { Tag(I, InList, OutList); });
  for (Value *P : Out)
    forEachAccess(P, [&](Instruction *I) { Tag(I, OutList, InList); });
}

/// The gather of an indexed vertex program, after the loop over its vertices
/// has written their outputs into the table at \p TabAt: for each index (four
/// to a quadword at \p IdxAt, UNPACK V4-8U, each the vertex times \p NOut,
/// its place in the table), the vertex's outputs are copied into the GIF
/// packet after the tag at \p Out. \p NQ is the number of index quadwords;
/// indices after the last one of the packet copy into the room behind it.
/// \p B is at the end of the vertex loop and is left at the end of the
/// gather.
void EntryLowering::buildGather(IRBuilder<> &B, Value *Top, Value *Out,
                                Value *NQ, uint64_t TabAt, uint64_t IdxAt,
                                unsigned NOut) {
  BasicBlock *Start = B.GetInsertBlock();
  BasicBlock *Loop = BasicBlock::Create(Ctx, "vu.gather", &F);
  BasicBlock *Kick = BasicBlock::Create(Ctx, "vu.kick", &F);
  Type *I16 = B.getInt16Ty();
  Type *QuadTy = FixedVectorType::get(B.getInt32Ty(), 4);
  PointerType *InTy = PointerType::get(Ctx, VUAS::Input);
  PointerType *OutTy = PointerType::get(Ctx, VUAS::Output);
  // The table is at a fixed address: it becomes the offset of the LQs.
  Value *Tab = B.getInt16(TabAt);
  Value *Idx = B.CreateAdd(Top, B.getInt16(IdxAt), "idx");
  Value *Pkt = B.CreateAdd(Out, B.getInt16(1), "pkt");
  B.CreateCondBr(B.CreateICmpEQ(NQ, B.getInt16(0), "noidx"), Kick, Loop);

  // The table and the indices are not written, the packet is not read: the
  // loads of the next indices can pass the stores of these.
  MDBuilder MDB(Ctx);
  MDNode *Domain = MDB.createAnonymousAliasScopeDomain(F.getName());
  MDNode *Read =
      MDNode::get(Ctx, {MDB.createAnonymousAliasScope(Domain, "gather.in")});
  MDNode *Write =
      MDNode::get(Ctx, {MDB.createAnonymousAliasScope(Domain, "gather.out")});
  auto Scope = [](Instruction *I, MDNode *S, MDNode *NoAlias) {
    I->setMetadata(LLVMContext::MD_alias_scope, S);
    I->setMetadata(LLVMContext::MD_noalias, NoAlias);
  };
  // The byte address of quadword QW plus Bytes; VULowerMemory takes the
  // scale off again.
  auto Ptr = [&](Value *QW, unsigned Bytes, PointerType *Ty) {
    Value *A = B.CreateShl(QW, 4);
    if (Bytes)
      A = B.CreateAdd(A, B.getInt16(Bytes));
    return B.CreateIntToPtr(A, Ty);
  };

  B.SetInsertPoint(Loop);
  PHINode *K = B.CreatePHI(I16, 2, "k");
  PHINode *PIdx = B.CreatePHI(I16, 2, "pidx");
  PHINode *PPkt = B.CreatePHI(I16, 2, "ppkt");
  K->addIncoming(B.getInt16(0), Start);
  PIdx->addIncoming(Idx, Start);
  PPkt->addIncoming(Pkt, Start);
  for (unsigned Field = 0; Field != 4; ++Field) {
    LoadInst *V = B.CreateLoad(I16, Ptr(PIdx, 4 * Field, InTy), "v");
    Scope(V, Read, Write);
    Value *At = B.CreateAdd(Tab, V, "at");
    for (unsigned R = 0; R != NOut; ++R) {
      LoadInst *Q = B.CreateLoad(
          QuadTy, Ptr(R ? B.CreateAdd(At, B.getInt16(R)) : At, 0, InTy));
      Scope(Q, Read, Write);
      StoreInst *S = B.CreateStore(
          Q, Ptr(B.CreateAdd(PPkt, B.getInt16(Field * NOut + R)), 0, OutTy));
      Scope(S, Write, Read);
    }
  }
  Value *KNext = B.CreateAdd(K, B.getInt16(1), "k.next");
  K->addIncoming(KNext, Loop);
  PIdx->addIncoming(B.CreateAdd(PIdx, B.getInt16(1)), Loop);
  PPkt->addIncoming(B.CreateAdd(PPkt, B.getInt16(4 * NOut)), Loop);
  B.CreateCondBr(B.CreateICmpEQ(KNext, NQ, "k.last"), Kick, Loop);
  B.SetInsertPoint(Kick);
}

/// The STMASK word that fills fields \p Fill from COL (binary 10 in each
/// write cycle), fields \p RowFill from ROW (01), and writes the others from
/// the data (00).
static uint32_t fillMask(unsigned Fill, unsigned RowFill) {
  uint32_t Cycle = 0;
  for (unsigned L = 0; L != 4; ++L) {
    if (Fill & (1 << L))
      Cycle |= 2u << (2 * L);
    if (RowFill & (1 << L))
      Cycle |= 1u << (2 * L);
  }
  return Cycle * 0x01010101u;
}

static std::string upper(StringRef S) {
  std::string R;
  for (char C : S)
    R += isAlnum(C) ? toUpper(C) : '_';
  return R;
}

bool EntryLowering::run(std::string &Header) {
  inlineCalls();
  collectGlobals();
  if (Failed)
    return true;
  Up = upper(F.getName());
  Prefix = F.getFnAttribute("vu-header-prefix").isValid()
               ? F.getFnAttribute("vu-header-prefix").getValueAsString().str()
               : "VU_";
  std::string P = Prefix + Up + "_";
  raw_string_ostream H(Header);

  uint64_t UniBytes = 0;
  for (const Slot &S : Uniforms)
    UniBytes = std::max(UniBytes, S.Offset + S.Bytes);
  uint64_t UniQW = (UniBytes + 15) / 16;
  uint64_t Spill = F.getFnAttributeAsParsedInteger("vu-spill-qw", 0);

  // The item index and count, used by the code.
  SmallVector<IntrinsicInst *, 4> IndexCalls, CountCalls;
  for (Instruction &I : instructions(F))
    if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
      if (II->getIntrinsicID() == Intrinsic::vu_item_index)
        IndexCalls.push_back(II);
      else if (II->getIntrinsicID() == Intrinsic::vu_item_count)
        CountCalls.push_back(II);
    }
  bool PerItem = Vertex || !IndexCalls.empty();

  if (Vertex && Items)
    error("a vertex program has inputs, not items");
  if (!Vertex && (!Inputs.empty() || !Outputs.empty()))
    error("a compute program has items, not inputs and outputs");
  if (Vertex && Inputs.empty())
    error("a vertex program needs inputs");
  if (Vertex && (Outputs.empty() || Outputs.size() > 15))
    error("a vertex program writes 1 to 15 GIF registers");
  uint64_t ItemQW = 0;
  if (Items) {
    Type *T = Items->getValueType();
    if (auto *AT = dyn_cast<ArrayType>(T))
      T = AT->getElementType();
    uint64_t Size = DL.getTypeAllocSize(T);
    if (Size % 16)
      error("an item is a whole number of quadwords");
    ItemQW = Size / 16;
  }
  if (Failed)
    return true;

  // The new entry block in front of the code; allocas stay in it.
  BasicBlock *Body = &F.getEntryBlock();
  BasicBlock *Entry = BasicBlock::Create(Ctx, "vu.entry", &F, Body);
  for (Instruction &I : make_early_inc_range(*Body))
    if (auto *AI = dyn_cast<AllocaInst>(&I); AI && AI->isStaticAlloca())
      AI->moveBefore(*Entry, Entry->end());
  // A vertex program starts at vu.entry for the first batch of a draw (MSCAL:
  // the init, which takes the context into registers, then the batch), and
  // at vu.batch for the others (MSCNT after the yield that ends a batch). Each
  // batch is one MSCAL or MSCNT, so the batches take the two buffers by turns.
  BasicBlock *Start =
      Vertex ? BasicBlock::Create(Ctx, "vu.batch", &F, Body) : Entry;
  IRBuilder<> B(Entry), SB(Start);
  Type *I16 = B.getInt16Ty();

  // The private globals: an alloca each, set at the start of each item (or
  // of each batch, for !vu.carry), and promoted to registers below.
  SmallVector<AllocaInst *, 4> Allocas;
  SmallVector<std::pair<AllocaInst *, GlobalVariable *>, 4> PerItemInit;
  for (GlobalVariable *GV : Privates) {
    AllocaInst *AI = B.CreateAlloca(GV->getValueType(), nullptr, GV->getName());
    Allocas.push_back(AI);
    // The alloca is in address space 0: the accesses are retargeted (the
    // other functions are inlined).
    retargetPointer(GV, AI);
    if (GV->getMetadata("vu.carry") || !PerItem) {
      if (GV->hasInitializer())
        SB.CreateStore(GV->getInitializer(), AI);
    } else {
      PerItemInit.push_back({AI, GV});
    }
  }

  Value *Top = nullptr, *Count, *Out = nullptr, *Tag = nullptr, *TagX = nullptr,
        *NQ = nullptr, *UniBase = nullptr, *CtxP = nullptr;
  // The batch's index counts and the context's GIF tag (vertex programs).
  std::function<void(IRBuilder<> &)> LoadHead;
  uint64_t BufQW = 0, MaxN = 0, Align = 1, InAt = 0, OutAt = 0;
  // An indexed vertex program: MaxN vertices, MaxI indices (see buildGather).
  uint64_t Indexed =
      Vertex ? F.getFnAttributeAsParsedInteger("vu-indexed", 0) : 0;
  uint64_t MaxI = 0, TabAt = 0, IdxAt = 0, EmptyAt = 0;
  // The contexts of a vertex program: CtxQW each, from CtxAt. Two let the EE
  // write the context of the next draw while the batches of this one run; one
  // needs a FLUSHE in front of it.
  uint64_t NCtx =
      Vertex ? F.getFnAttributeAsParsedInteger("vu-contexts", 2) : 0;
  uint64_t CtxQW = 1 + UniQW, CtxAt = 0;
  if (Vertex && NCtx != 1 && NCtx != 2)
    error("a vertex program has one or two contexts");
  // The quadword in front of the inputs: the counts.
  uint64_t Head = 1;
  // A value carried from vertex to vertex needs the order in which the
  // vertices are drawn; an indexed program transforms each vertex once, in
  // the order of its table.
  if (Indexed)
    for (GlobalVariable *GV : Privates)
      if (GV->getMetadata("vu.carry"))
        error("an indexed vertex program cannot carry " + GV->getName() +
              " from vertex to vertex");
  if (Vertex) {
    // XTOP and XGKICK: VU1 only.
    Attribute CPU = F.getFnAttribute("target-cpu");
    if (CPU.isValid() && CPU.getValueAsString() != "vu1")
      error("vertex programs run on VU1");
    F.addFnAttr("target-cpu", "vu1");
    uint64_t NIn = Inputs.size(), NOut = Outputs.size();
    if (!Indexed) {
      CtxAt = 1024 - Spill - NCtx * CtxQW;
      BufQW = F.getFnAttributeAsParsedInteger("vu-buffer-qw", CtxAt / 2);
      if (2 * BufQW > CtxAt)
        error("two buffers of " + Twine(BufQW) + " quadwords, " + Twine(NCtx) +
              " contexts of " + Twine(CtxQW) + " and " + Twine(Spill) +
              " for spills do not fit VU1 data memory");
    } else if (F.hasFnAttribute("vu-buffer-qw")) {
      error("an indexed vertex program sizes its buffers itself");
    }
    for (const Slot &S : Inputs)
      Align = std::max<uint64_t>(Align, 16 / std::gcd(16u, S.Bytes));
    // Batches start on a quadword of every input stream, and an UNPACK ends
    // on one, so up to Align - 1 more vertices are unpacked.
    InAt = Head;
    if (!Indexed) {
      int64_t Room = int64_t(BufQW) - 2 - (Align - 1) * NIn;
      MaxN = Room > 0 ? Room / (NIn + NOut) : 0;
      OutAt = InAt + (MaxN + Align - 1) * NIn;
      if (MaxN < Align || OutAt + 1 + MaxN * NOut > BufQW || OutAt > 1023)
        error("the buffer of " + Twine(BufQW) +
              " quadwords has no room for a batch");
    } else {
      // Each of the two buffers has the counts, the inputs and the indices
      // (four to a quadword). Above them, under the spills, what the batches
      // share:
      //  - the GIF packet: one, as a batch waits with an XGKICK of the empty
      //    GIF tag until the GIF has read the packet of the batch before
      //    (while its vertex loop ran), before the gather writes it;
      //  - the empty GIF tag;
      //  - the contexts (see CtxAt; the counts name one). With two, the EE
      //    fills them by turns, one per draw: a context is not written while
      //    a batch reads it, as VIF1 gets to the context of draw d + 2 after
      //    the MSCALs of draw d + 1, which wait until the batches of draw d
      //    have ended;
      //  - the table: a batch writes and reads it before its XGKICK.
      // What the inputs leave is for the indices: four quadwords in each
      // buffer and 16 * NOut of packet for every 16 (the EE sends them in
      // whole quadwords of 16 bytes, which UNPACK V4-8 makes four quadwords).
      MaxN = Indexed;
      uint64_t TabQW = MaxN * NOut;
      IdxAt = InAt + (MaxN + Align - 1) * NIn;
      TabAt = 1024 - Spill - TabQW;
      CtxAt = TabAt - NCtx * CtxQW;
      EmptyAt = CtxAt - 1;
      int64_t Room = int64_t(EmptyAt) - 1 - 2 * int64_t(IdxAt);
      MaxI = Room > 0 ? Room / (8 + 16 * NOut) * 16 : 0;
      BufQW = IdxAt + MaxI / 4;
      OutAt = 2 * BufQW;
      if (MaxI < 16 || OutAt + 1 + MaxI * NOut > EmptyAt)
        error("VU1 data memory has no room for batches of " + Twine(MaxN) +
              " indexed vertices");
      // An index is a byte: the vertex's place in the table.
      if ((MaxN - 1) * NOut > 255)
        error(Twine(MaxN) + " vertices of " + Twine(NOut) +
              " outputs are too many for byte indices");
    }
    if (Failed)
      return true;
    // The init: the context at ITOP.
    CtxP = B.CreateIntrinsic(Intrinsic::vu_xitop, {}, {}, nullptr, "ctx");
    if (UniQW)
      UniBase = B.CreateAdd(CtxP, B.getInt16(1), "uni");
    B.CreateBr(Start);

    // A batch: the counts at XTOP, x the vertices, z the index quadwords and
    // w the indices (indexed).
    B.SetInsertPoint(Start);
    Top = B.CreateIntrinsic(Intrinsic::vu_xtop, {}, {}, nullptr, "top");
    auto Field = [&](IRBuilder<> &B, unsigned K, const Twine &Name) {
      return B.CreateIntrinsic(Intrinsic::vu_ilw, {}, {Top, B.getInt32(K)},
                               nullptr, Name);
    };
    Count = Field(B, 0, "n");
    // The context's GIF tag, loaded in each batch (the context is not
    // written while its batches run; each MSCNT sets ITOP again): no
    // registers are kept for it across the batches. It has NLOOP 0, so the
    // low half of x is the EOP bit.
    LoadHead = [&, Field](IRBuilder<> &B) {
      Value *NLoop = Count;
      if (Indexed) {
        NQ = Field(B, 2, "nq");
        NLoop = Field(B, 3, "ni");
      }
      Value *CtxB =
          B.CreateIntrinsic(Intrinsic::vu_xitop, {}, {}, nullptr, "ctx");
      Tag = B.CreateIntrinsic(Intrinsic::vu_lq, {}, {CtxB}, nullptr, "tag");
      Value *EOP = B.CreateIntrinsic(Intrinsic::vu_ilw, {},
                                     {CtxB, B.getInt32(0)}, nullptr, "eop");
      TagX = B.CreateOr(EOP, NLoop, "tag.x");
    };
    // Indexed, the head is only needed after the vertices, for the gather
    // (vu.done): it is loaded there, not kept in registers through the loop.
    if (!Indexed)
      LoadHead(B);
    Out = Indexed ? static_cast<Value *>(B.getInt16(OutAt))
                  : B.CreateAdd(Top, B.getInt16(OutAt), "out");
  } else {
    Count = B.CreateIntrinsic(Intrinsic::vu_ilw, {},
                              {B.getInt16(0), B.getInt32(0)}, nullptr, "n");
  }

  // Uniforms after the GIF tag of the context, or from quadword 1 of memory.
  if (!UniBase)
    UniBase = B.getInt16(1);
  SmallVector<Value *, 8> InPtrs, OutPtrs;
  for (const Slot &S : Uniforms) {
    Value *Ptr = qwPointer(B, S.GV, UniBase, S.Offset);
    replaceGlobal(S.GV, Ptr);
    InPtrs.push_back(Ptr);
    forEachAccess(Ptr, [&](Instruction *I) {
      if (isa<LoadInst>(I))
        I->setMetadata("vu.uniform.load", MDNode::get(Ctx, {}));
      else
        error("uniform " + S.GV->getName() + " is written");
    });
  }
  uint64_t DataQW = 1 + UniQW;
  if (Items) {
    Value *ItemsPtr = qwPointer(B, Items, B.getInt16(DataQW));
    replaceGlobal(Items, ItemsPtr);
    // A compute program reads and writes its items, and only reads the
    // uniforms (addAliasScopes).
    OutPtrs.push_back(ItemsPtr);
  }
  for (IntrinsicInst *II : CountCalls) {
    II->replaceAllUsesWith(Count);
    II->eraseFromParent();
  }

  SmallVector<PHINode *, 3> LoopPhis;
  if (PerItem) {
    // vu.entry -> vu.item (phis) -> code ... -> vu.next -> vu.item / vu.done
    BasicBlock *Item = BasicBlock::Create(Ctx, "vu.item", &F, Body);
    BasicBlock *Next = BasicBlock::Create(Ctx, "vu.next", &F);
    BasicBlock *Done = BasicBlock::Create(Ctx, "vu.done", &F);
    Value *Empty = B.CreateICmpEQ(Count, B.getInt16(0), "empty");
    B.CreateCondBr(Empty, Done, Item);

    B.SetInsertPoint(Item);
    PHINode *I = B.CreatePHI(I16, 2, "i");
    I->addIncoming(B.getInt16(0), Start);
    PHINode *PIn = nullptr, *POut = nullptr;
    LoopPhis.push_back(I);
    if (Vertex) {
      PIn = B.CreatePHI(I16, 2, "pi");
      POut = B.CreatePHI(I16, 2, "po");
      LoopPhis.append({PIn, POut});
      // The start values, in front of the batch's branch.
      IRBuilder<> EB(Start->getTerminator());
      PIn->addIncoming(EB.CreateAdd(Top, EB.getInt16(InAt), "in"), Start);
      // Indexed: the outputs go to the table, for the gather.
      POut->addIncoming(Indexed ? static_cast<Value *>(EB.getInt16(TabAt))
                                : EB.CreateAdd(Out, EB.getInt16(1), "o1"),
                        Start);
    }
    for (auto [AI, GV] : PerItemInit)
      if (GV->hasInitializer())
        B.CreateStore(GV->getInitializer(), AI);
    if (Vertex) {
      for (auto [K, S] : enumerate(Inputs)) {
        Value *Ptr = qwPointer(B, S.GV, B.CreateAdd(PIn, B.getInt16(K)));
        replaceGlobal(S.GV, Ptr);
        InPtrs.push_back(Ptr);
      }
      for (auto [K, S] : enumerate(Outputs)) {
        dropIgnoredFields(S);
        Value *Ptr = qwPointer(B, S.GV, B.CreateAdd(POut, B.getInt16(K)));
        replaceGlobal(S.GV, Ptr);
        OutPtrs.push_back(Ptr);
      }
    }
    B.CreateBr(Body);
    for (IntrinsicInst *II : IndexCalls) {
      II->replaceAllUsesWith(I);
      II->eraseFromParent();
    }

    // The end of the code for one item goes to the next item.
    for (BasicBlock &BB : F)
      if (auto *RI = BB.empty() ? nullptr : dyn_cast<ReturnInst>(&BB.back())) {
        UncondBrInst::Create(Next, &BB);
        RI->eraseFromParent();
      }
    B.SetInsertPoint(Next);
    Value *INext = B.CreateAdd(I, B.getInt16(1), "i.next");
    I->addIncoming(INext, Next);
    if (Vertex) {
      PIn->addIncoming(B.CreateAdd(PIn, B.getInt16(Inputs.size())), Next);
      POut->addIncoming(B.CreateAdd(POut, B.getInt16(Outputs.size())), Next);
    }
    B.CreateCondBr(B.CreateICmpEQ(INext, Count, "last"), Done, Item);

    B.SetInsertPoint(Done);
    if (Indexed) {
      // XTOP is the same until the next MSCNT.
      Top = B.CreateIntrinsic(Intrinsic::vu_xtop, {}, {}, nullptr, "top");
      LoadHead(B);
      // Wait until the GIF has read the packet of the batch before: the
      // empty tag (NLOOP 0, EOP) is sent after it.
      Value *Empty = B.getInt16(EmptyAt);
      B.CreateIntrinsic(Intrinsic::vu_isw, {},
                        {B.getInt16(0x8000), Empty, B.getInt32(0)});
      B.CreateIntrinsic(Intrinsic::vu_isw, {},
                        {B.getInt16(0), Empty, B.getInt32(1)});
      B.CreateIntrinsic(Intrinsic::vu_xgkick, {}, {Empty});
      B.CreateIntrinsic(Intrinsic::vu_sq, {}, {Tag, Out});
      B.CreateIntrinsic(Intrinsic::vu_isw, {}, {TagX, Out, B.getInt32(0)});
      buildGather(B, Top, Out, NQ, TabAt, IdxAt, Outputs.size());
    }
    if (Vertex) {
      // The batch ends; MSCNT starts the next one.
      B.CreateIntrinsic(Intrinsic::vu_xgkick, {}, {Out});
      B.CreateIntrinsic(Intrinsic::vu_yield, ArrayRef<Type *>(),
                        ArrayRef<Value *>());
      B.CreateBr(Start);
    } else {
      B.CreateRetVoid();
    }

    // A straight-line item is a loop of one block, which the pipeliner takes.
    MergeBlockIntoPredecessor(Body);
    if (BasicBlock *Pred = Next->getSinglePredecessor();
        Pred && Pred->getSingleSuccessor() == Next)
      MergeBlockIntoPredecessor(Next);
  } else {
    B.CreateBr(Body);
  }

  if (!OutPtrs.empty())
    addAliasScopes(InPtrs, OutPtrs);
  hoistToEntry(Entry);
  // The GIF tag in front of the outputs, at the start of each batch.
  if (Tag && !Indexed) {
    IRBuilder<> EB(Start->getTerminator());
    EB.CreateIntrinsic(Intrinsic::vu_sq, {}, {Tag, Out});
    EB.CreateIntrinsic(Intrinsic::vu_isw, {}, {TagX, Out, EB.getInt32(0)});
  }
  if (!Allocas.empty()) {
    DominatorTree DT(F);
    PromoteMemToReg(Allocas, DT);
    // The item counters first, then the values carried from item to item.
    for (PHINode *Phi : reverse(LoopPhis))
      Phi->moveBefore(Phi->getParent()->begin());
  }

  // The layout for the EE.
  H << "/* " << (Vertex ? "vertex program " : "compute program ") << F.getName()
    << ": VU data memory layout, in quadwords */\n";
  for (const Slot &S : Uniforms) {
    H << "#define " << P << upper(S.GV->getName()) << " "
      << (Vertex ? 0 : 1) + S.Offset / 16;
    if (S.Offset % 16)
      H << "  /* field " << "xyzw"[S.Offset % 16 / 4] << " */";
    H << "\n";
  }
  H << "#define " << P << "ENTRY 0  /* micro address, 64-bit units"
    << (Vertex ? "; MSCAL (ITOP = the context) for the first batch of a "
                 "draw, MSCNT for the others"
               : "")
    << " */\n";
  if (Vertex) {
    uint64_t Regs = 0;
    for (auto [K, S] : enumerate(Outputs))
      Regs |= uint64_t(S.Code) << (4 * K);
    H << "#define " << P << "BUF_QW " << BufQW
      << "  /* double buffer size */\n";
    H << "#define " << P << "UNI_QW " << UniQW
      << "  /* uniforms, after the context's GIF tag */\n";
    H << "#define " << P << "CTX_AT " << CtxAt
      << "  /* the contexts: GIF tag (NLOOP 0), uniforms */\n";
    H << "#define " << P << "CTX_QW " << CtxQW << "  /* per context */\n";
    H << "#define " << P << "NCTX " << NCtx
      << "  /* contexts (1: a FLUSHE before writing it) */\n";
    H << "#define " << P << "ITEM_QW " << Inputs.size()
      << "  /* quadwords per vertex in */\n";
    H << "#define " << P << "OUT_QW " << Outputs.size()
      << "  /* GIF registers per vertex */\n";
    H << "#define " << P << "MAX_N " << MaxN << "  /* vertices per batch */\n";
    H << "#define " << P << "ALIGN " << Align
      << "  /* batches start at multiples of it */\n";
    H << "#define " << P
      << "COUNTS 0  /* x vertices, z index quadwords, w indices */\n";
    H << "#define " << P << "IN_AT " << InAt << "  /* first item */\n";
    H << "#define " << P << "OUT_AT " << OutAt
      << (Indexed ? "  /* the output GIF tag, one for both buffers */\n"
                  : "  /* the output GIF tag */\n");
    if (Indexed) {
      H << "#define " << P << "INDEXED 1\n";
      H << "#define " << P << "EMPTY_AT " << EmptyAt
        << "  /* the empty GIF tag */\n";
      H << "#define " << P << "MAX_I " << MaxI
        << "  /* indices per batch, a multiple of 16 */\n";
      H << "#define " << P << "TAB_AT " << TabAt
        << "  /* the outputs of the vertices */\n";
      H << "#define " << P << "IDX_AT " << IdxAt
        << "  /* the indices, V4-8U, each vertex * OUT_QW */\n";
    }
    H << "#define " << P << "NREG " << Outputs.size() << "\n";
    H << "#define " << P << "REGS 0x" << utohexstr(Regs, /*LowerCase=*/true)
      << "ull  /* GIF register list */\n";
    H << "#define " << P << "NIN " << Inputs.size() << "\n";
    for (auto [K, S] : enumerate(Inputs))
      H << "#define " << P << "IN_" << upper(S.GV->getName()) << " " << K
        << "  /* " << S.Kind << ", " << S.Bytes << " bytes */\n";
    H << "#define " << P << "INPUTS {";
    for (auto [K, S] : enumerate(Inputs))
      H << (K ? ", " : "") << "{0x" << utohexstr(S.Code, true) << ", "
        << S.Unsigned << ", " << S.Bytes << ", 0x"
        << utohexstr(fillMask(S.Fill, S.RowFill), true) << "}";
    H << "}  /* UNPACK format, unsigned, bytes, STMASK (fields 1.0 from COL, "
         "values from ROW) */\n";
    H << "#define " << P << "ROW {" << Row[0] << ", " << Row[1] << ", "
      << Row[2] << ", " << Row[3] << "}  /* ROW of the masked UNPACKs */\n";
  } else {
    H << "#define " << P << "N 0  /* the item count, field x */\n";
    if (Items)
      H << "#define " << P << upper(Items->getName()) << " " << DataQW
        << "  /* the items */\n";
    H << "#define " << P << "SIZE " << DataQW
      << "  /* quadwords before the items */\n";
    H << "#define " << P << "ITEM_QW " << ItemQW
      << "  /* quadwords per item */\n";
    H << "#define " << P << "FORMAT 0x0C  /* UNPACK V4-32 */\n";
    H << "#define " << P << "DBUF 0  /* no double buffering */\n";
  }
  if (Spill)
    H << "#define " << P << "SPILL_QW " << Spill
      << "  /* at the top of data memory */\n";
  H << "\n";

  F.removeFnAttr("vu-entry");
  for (Slot &S : Uniforms)
    S.GV->removeDeadConstantUsers();
  return true;
}

static bool lowerEntries(Module &M) {
  std::string Header;
  bool Changed = false;
  unsigned NumEntries = 0;
  for (Function &F : M) {
    Attribute A = F.getFnAttribute("vu-entry");
    if (!A.isValid() || F.isDeclaration())
      continue;
    StringRef Kind = A.getValueAsString();
    if (Kind != "vertex" && Kind != "compute") {
      M.getContext().diagnose(DiagnosticInfoUnsupported(
          F, "VU: \"vu-entry\" is \"vertex\" or \"compute\""));
      continue;
    }
    if (++NumEntries > 1)
      M.getContext().diagnose(
          DiagnosticInfoUnsupported(F, "VU: one entry function per module"));
    EntryLowering L(M, F, Kind == "vertex");
    Changed |= L.run(Header);
    // A program with errors is not compiled: what is left of it has
    // accesses that the later passes do not take.
    if (L.failed()) {
      F.deleteBody();
      F.removeFnAttr("vu-entry");
      Changed = true;
    }
  }
  if (!Changed)
    return false;
  // The globals are gone, and so are the functions that were inlined.
  for (GlobalVariable &GV : make_early_inc_range(M.globals())) {
    GV.removeDeadConstantUsers();
    if (GV.use_empty() && GV.getAddressSpace() >= VUAS::Uniform &&
        GV.getAddressSpace() <= VUAS::Private)
      GV.eraseFromParent();
  }
  for (Function &F : make_early_inc_range(M))
    if (!F.isDeclaration() && F.use_empty() && F.hasLocalLinkage())
      F.eraseFromParent();
  // The header goes into a section of its own, for llvm-objcopy
  // --dump-section .vu.header=<file>.
  if (!Header.empty()) {
    Constant *Str = ConstantDataArray::getString(M.getContext(), Header,
                                                 /*AddNull=*/false);
    auto *GV =
        new GlobalVariable(M, Str->getType(), /*isConstant=*/true,
                           GlobalValue::PrivateLinkage, Str, "vu.header");
    GV->setSection(".vu.header");
    GV->setAlignment(llvm::Align(1));
    appendToUsed(M, {GV});
  }
  return true;
}

namespace {
class VUEntryLoweringLegacy : public ModulePass {
public:
  static char ID;
  VUEntryLoweringLegacy() : ModulePass(ID) {}
  StringRef getPassName() const override { return "VU entry lowering"; }
  bool runOnModule(Module &M) override { return lowerEntries(M); }
};
} // namespace

char VUEntryLoweringLegacy::ID = 0;

INITIALIZE_PASS(VUEntryLoweringLegacy, DEBUG_TYPE, "VU entry lowering", false,
                false)

ModulePass *llvm::createVUEntryLoweringPass() {
  return new VUEntryLoweringLegacy();
}

PreservedAnalyses VUEntryLoweringPass::run(Module &M, ModuleAnalysisManager &) {
  return lowerEntries(M) ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
