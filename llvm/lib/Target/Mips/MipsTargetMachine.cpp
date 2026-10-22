//===-- MipsTargetMachine.cpp - Define TargetMachine for Mips -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements the info about Mips target spec.
//
//===----------------------------------------------------------------------===//

#include "MipsTargetMachine.h"
#include "MCTargetDesc/MipsABIInfo.h"
#include "MCTargetDesc/MipsMCTargetDesc.h"
#include "Mips.h"
#include "Mips16ISelDAGToDAG.h"
#include "MipsDAGMutations.h"
#include "MipsMachineFunction.h"
#include "MipsR5900VU0MatVec.h"
#include "MipsSEISelDAGToDAG.h"
#include "MipsSubtarget.h"
#include "MipsTargetObjectFile.h"
#include "MipsTargetTransformInfo.h"
#include "TargetInfo/MipsTargetInfo.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/ScalarEvolutionAliasAnalysis.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/BasicTTIImpl.h"
#include "llvm/CodeGen/GlobalISel/CSEInfo.h"
#include "llvm/CodeGen/GlobalISel/IRTranslator.h"
#include "llvm/CodeGen/GlobalISel/InstructionSelect.h"
#include "llvm/CodeGen/GlobalISel/Legalizer.h"
#include "llvm/CodeGen/GlobalISel/RegBankSelect.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineScheduler.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Function.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Option/LibraryOptions.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetOptions.h"
#include <optional>
#include <string>

using namespace llvm;

#define DEBUG_TYPE "mips"

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeMipsTarget() {
  static opt::RegisterLibraryOptions<MipsOptions> O;
  // Register the target.
  RegisterTargetMachine<MipsebTargetMachine> X(getTheMipsTarget());
  RegisterTargetMachine<MipselTargetMachine> Y(getTheMipselTarget());
  RegisterTargetMachine<MipsebTargetMachine> A(getTheMips64Target());
  RegisterTargetMachine<MipselTargetMachine> B(getTheMips64elTarget());

  PassRegistry *PR = PassRegistry::getPassRegistry();
  initializeGlobalISel(*PR);
  initializeMipsAsmPrinterPass(*PR);
  initializeMipsDelaySlotFillerPass(*PR);
  initializeMipsBranchExpansionPass(*PR);
  initializeMicroMipsSizeReducePass(*PR);
  initializeMipsPreLegalizerCombinerPass(*PR);
  initializeMipsPostLegalizerCombinerPass(*PR);
  initializeMipsMulMulBugFixPass(*PR);
  initializeMipsR5900Div1Pass(*PR);
  initializeMipsR5900Mult1Pass(*PR);
  initializeMipsR5900FPUAccChainPass(*PR);
  initializeMipsR5900IssuePass(*PR);
  initializeMipsR5900VU0AccChainPass(*PR);
  initializeMipsDAGToDAGISelLegacyPass(*PR);
}

static std::unique_ptr<TargetLoweringObjectFile>
createTLOF(const Triple &TT, bool UseSmallSection) {
  if (TT.isOSBinFormatCOFF())
    return std::make_unique<TargetLoweringObjectFileCOFF>();
  return std::make_unique<MipsTargetObjectFile>(UseSmallSection);
}

static Reloc::Model getEffectiveRelocModel(std::optional<Reloc::Model> RM) {
  return RM.value_or(Reloc::Static);
}

// On function prologue, the stack is created by decrementing
// its pointer. Once decremented, all references are done with positive
// offset from the stack/frame pointer, using StackGrowsUp enables
// an easier handling.
// Using CodeModel::Large enables different CALL behavior.
MipsTargetMachine::MipsTargetMachine(const Target &T, const Triple &TT,
                                     StringRef CPU, StringRef FS,
                                     const TargetOptions &Options,
                                     std::optional<Reloc::Model> RM,
                                     std::optional<CodeModel::Model> CM,
                                     CodeGenOptLevel OL, bool JIT,
                                     bool isLittle)
    : CodeGenTargetMachineImpl(T, TT, CPU, FS, Options,
                               getEffectiveRelocModel(RM),
                               getEffectiveCodeModel(CM, CodeModel::Small), OL),
      CLOpts(cl::LibraryOptionsContext::get<MipsOptions>(
          Options.MCOptions.LibraryOptions)),
      MCCLOpts(MIPS_MC::getCLOpts(Options.MCOptions)), isLittle(isLittle),
      IsJIT(JIT), DefaultSubtarget(TT, CPU, FS, Options.MCOptions.getABIName(),
                                   isLittle, *this, std::nullopt),
      TLOF(createTLOF(TT, DefaultSubtarget.useSmallSection())) {
  initAsmInfo();

  // Mips supports the debug entry values.
  setSupportsDebugEntryValues(true);
}

MipsTargetMachine::~MipsTargetMachine() = default;

void MipsebTargetMachine::anchor() {}

MipsebTargetMachine::MipsebTargetMachine(const Target &T, const Triple &TT,
                                         StringRef CPU, StringRef FS,
                                         const TargetOptions &Options,
                                         std::optional<Reloc::Model> RM,
                                         std::optional<CodeModel::Model> CM,
                                         CodeGenOptLevel OL, bool JIT)
    : MipsTargetMachine(T, TT, CPU, FS, Options, RM, CM, OL, JIT, false) {}

void MipselTargetMachine::anchor() {}

MipselTargetMachine::MipselTargetMachine(const Target &T, const Triple &TT,
                                         StringRef CPU, StringRef FS,
                                         const TargetOptions &Options,
                                         std::optional<Reloc::Model> RM,
                                         std::optional<CodeModel::Model> CM,
                                         CodeGenOptLevel OL, bool JIT)
    : MipsTargetMachine(T, TT, CPU, FS, Options, RM, CM, OL, JIT, true) {}

const MipsSubtarget *
MipsTargetMachine::getSubtargetImpl(const Function &F) const {
  Attribute CPUAttr = F.getFnAttribute("target-cpu");
  Attribute FSAttr = F.getFnAttribute("target-features");

  std::string CPU =
      CPUAttr.isValid() ? CPUAttr.getValueAsString().str() : TargetCPU;
  std::string FS =
      FSAttr.isValid() ? FSAttr.getValueAsString().str() : TargetFS;
  bool hasMips16Attr = F.getFnAttribute("mips16").isValid();
  bool hasNoMips16Attr = F.getFnAttribute("nomips16").isValid();

  bool HasMicroMipsAttr = F.getFnAttribute("micromips").isValid();
  bool HasNoMicroMipsAttr = F.getFnAttribute("nomicromips").isValid();

  // FIXME: This is related to the code below to reset the target options,
  // we need to know whether or not the soft float flag is set on the
  // function, so we can enable it as a subtarget feature.
  bool softFloat = F.getFnAttribute("use-soft-float").getValueAsBool();

  if (hasMips16Attr)
    FS += FS.empty() ? "+mips16" : ",+mips16";
  else if (hasNoMips16Attr)
    FS += FS.empty() ? "-mips16" : ",-mips16";
  if (HasMicroMipsAttr)
    FS += FS.empty() ? "+micromips" : ",+micromips";
  else if (HasNoMicroMipsAttr)
    FS += FS.empty() ? "-micromips" : ",-micromips";
  if (softFloat)
    FS += FS.empty() ? "+soft-float" : ",+soft-float";

  StringRef ABIName = getTargetABIName(*F.getParent());

  auto &I = SubtargetMap[CPU + FS + ABIName.str()];
  if (!I) {
    I = std::make_unique<MipsSubtarget>(
        TargetTriple, CPU, FS, ABIName, isLittle, *this,
        MaybeAlign(F.getParent()->getOverrideStackAlignment()));
  }
  return I.get();
}

namespace {

/// Mips Code Generator Pass Configuration Options.
class MipsPassConfig : public TargetPassConfig {
public:
  MipsPassConfig(MipsTargetMachine &TM, PassManagerBase &PM)
      : TargetPassConfig(TM, PM) {
    // The current implementation of long branch pass requires a scratch
    // register ($at) to be available before branch instructions. Tail merging
    // can break this requirement.
    EnableTailMerge = false;
    EnableLoopTermFold = true;
  }

  MipsTargetMachine &getMipsTargetMachine() const {
    return getTM<MipsTargetMachine>();
  }

  /// The R5900 schedulers use SCEV based alias analysis.
  bool useSCEVAA() const {
    return getOptLevel() != CodeGenOptLevel::None &&
           TM->getTargetCPU() == "r5900";
  }

  void addIRPasses() override;
  bool addInstSelector() override;
  void addPreEmitPass() override;
  void addPreRegAlloc() override;
  void addPreSched2() override;
  bool addIRTranslator() override;
  void addPreLegalizeMachineIR() override;
  bool addLegalizeMachineIR() override;
  void addPreRegBankSelect() override;
  bool addRegBankSelect() override;
  bool addGlobalInstructionSelect() override;

  std::unique_ptr<CSEConfigBase> getCSEConfig() const override;
};

} // end anonymous namespace

void MipsTargetMachine::registerPassBuilderCallbacks(PassBuilder &PB) {
#define GET_PASS_REGISTRY "MipsPassRegistry.def"
#include "llvm/Passes/TargetPassRegistry.inc"

  // Before the vectorizers, which do not make VU0 matrix times vector code.
  PB.registerVectorizerStartEPCallback(
      [this](FunctionPassManager &FPM, OptimizationLevel) {
        FPM.addPass(MipsR5900VU0MatVecPass(*this));
      });
}

TargetPassConfig *MipsTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new MipsPassConfig(*this, PM);
}

std::unique_ptr<CSEConfigBase> MipsPassConfig::getCSEConfig() const {
  return getStandardCSEConfigForOpt(TM->getOptLevel());
}

void MipsPassConfig::addIRPasses() {
  TargetPassConfig::addIRPasses();
  addPass(createAtomicExpandLegacyPass());
  if (getMipsTargetMachine().getCLOpts().os16)
    addPass(createMipsOs16Pass());
  addPass(createMips16HardFloatPass());
}
// Install an instruction selector pass using
// the ISelDag to gen Mips code.
bool MipsPassConfig::addInstSelector() {
  // R5900: The schedulers use alias analysis to move loads above stores. SCEV
  // based alias analysis also tells apart accesses to different arrays of one
  // object that are indexed by the loop counter, such as a table lookup and the
  // store of its result. It is added right before instruction selection, as
  // the IR passes before it do not preserve it; the machine passes do, and the
  // R5900 issue pass keeps it alive.
  if (useSCEVAA())
    addPass(createSCEVAAWrapperPass());
  addPass(createMips16ISelDag(getMipsTargetMachine(), getOptLevel()));
  addPass(createMipsSEISelDag(getMipsTargetMachine(), getOptLevel()));
  return false;
}

void MipsPassConfig::addPreRegAlloc() {
  addPass(createMipsOptimizePICCallPass());

  // R5900: Optimize multiply-add chains to use the FPU and VU0 accumulators.
  // These must run before register allocation so virtual registers are still
  // in SSA form (one definition per register). Both passes skip functions
  // whose subtarget is not R5900 (or lacks VU0).
  addPass(createMipsR5900FPUAccChainPass());
  addPass(createMipsR5900VU0AccChainPass());
  // R5900: Use the pipe 1 divider for independent divides, and the pipe 1
  // multiplier where it lowers the resource bound of a block.
  addPass(createMipsR5900Div1Pass());
  addPass(createMipsR5900Mult1Pass());
}

void MipsPassConfig::addPreSched2() {
  // The post-RA MachineScheduler only runs for subtargets that enable it
  // (the R5900); the others keep using the PostRAScheduler.
  if (getOptLevel() != CodeGenOptLevel::None)
    addPass(&PostMachineSchedulerID);
}

ScheduleDAGInstrs *
MipsTargetMachine::createMachineScheduler(MachineSchedContext *C) const {
  ScheduleDAGMILive *DAG = createSchedLive(C);
  if (C->MF->getSubtarget<MipsSubtarget>().isR5900())
    DAG->addMutation(createMipsAccumulatorDAGMutation());
  return DAG;
}

ScheduleDAGInstrs *
MipsTargetMachine::createPostMachineScheduler(MachineSchedContext *C) const {
  ScheduleDAGMI *DAG = createSchedPostRA(C);
  if (C->MF->getSubtarget<MipsSubtarget>().isR5900())
    DAG->addMutation(createMipsLoadOrderDAGMutation());
  return DAG;
}

TargetTransformInfo
MipsTargetMachine::getTargetTransformInfo(const Function &F) const {
  LLVM_DEBUG(errs() << "Target Transform Info Pass Added\n");
  return TargetTransformInfo(std::make_unique<MipsTTIImpl>(this, F));
}

MachineFunctionInfo *MipsTargetMachine::createMachineFunctionInfo(
    BumpPtrAllocator &Allocator, const Function &F,
    const TargetSubtargetInfo *STI) const {
  return MipsFunctionInfo::create<MipsFunctionInfo>(Allocator, F, STI);
}

// Implemented by targets that want to run passes immediately before
// machine code is emitted.
void MipsPassConfig::addPreEmitPass() {
  // Expand pseudo instructions that are sensitive to register allocation.
  addPass(createMipsExpandPseudoPass());

  // The microMIPS size reduction pass performs instruction reselection for
  // instructions which can be remapped to a 16 bit instruction.
  addPass(createMicroMipsSizeReducePass());

  // This pass inserts a nop instruction between two back-to-back multiplication
  // instructions when the "mfix4300" flag is passed.
  if (getMipsTargetMachine().getCLOpts().mfix4300)
    addPass(createMipsMulMulBugPass());

  // The delay slot filler pass can potientially create forbidden slot hazards
  // for MIPSR6 and therefore it should go before MipsBranchExpansion pass.
  addPass(createMipsDelaySlotFillerPass());

  // The R5900 dual-issues the instructions of aligned 8-byte blocks, so it is
  // scheduled again once the addresses of the instructions are known.
  if (getOptLevel() != CodeGenOptLevel::None)
    addPass(createMipsR5900IssuePass(/*KeepSCEVAA=*/useSCEVAA()));

  // This pass expands branches and takes care about the forbidden slot hazards.
  // Expanding branches may potentially create forbidden slot hazards for
  // MIPSR6, and fixing such hazard may potentially break a branch by extending
  // its offset out of range. That's why this pass combine these two tasks, and
  // runs them alternately until one of them finishes without any changes. Only
  // then we can be sure that all branches are expanded properly and no hazards
  // exists.
  // Any new pass should go before this pass.
  addPass(createMipsBranchExpansion());

  addPass(createMipsConstantIslandPass());
}

bool MipsPassConfig::addIRTranslator() {
  addPass(new IRTranslatorLegacy(getOptLevel()));
  return false;
}

void MipsPassConfig::addPreLegalizeMachineIR() {
  addPass(createMipsPreLegalizeCombiner());
}

bool MipsPassConfig::addLegalizeMachineIR() {
  addPass(new LegalizerLegacy());
  return false;
}

void MipsPassConfig::addPreRegBankSelect() {
  bool IsOptNone = getOptLevel() == CodeGenOptLevel::None;
  addPass(createMipsPostLegalizeCombiner(IsOptNone));
}

bool MipsPassConfig::addRegBankSelect() {
  addPass(new RegBankSelectLegacy());
  return false;
}

bool MipsPassConfig::addGlobalInstructionSelect() {
  addPass(new InstructionSelectLegacy(getOptLevel()));
  return false;
}
