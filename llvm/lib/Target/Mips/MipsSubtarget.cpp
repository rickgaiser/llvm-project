//===-- MipsSubtarget.cpp - Mips Subtarget Information --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the Mips specific subclass of TargetSubtargetInfo.
//
//===----------------------------------------------------------------------===//

#include "MipsSubtarget.h"
#include "Mips.h"
#include "MipsCallLowering.h"
#include "MipsDAGMutations.h"
#include "MipsLegalizerInfo.h"
#include "MipsRegisterBankInfo.h"
#include "MipsRegisterInfo.h"
#include "MipsSelectionDAGInfo.h"
#include "MipsTargetMachine.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachinePipeliner.h"
#include "llvm/CodeGen/SchedulerRegistry.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/CodeGen/SingleRegClassMutations.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Option/LibraryOptions.h"
#include "llvm/Support/BranchProbability.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "mips-subtarget"

#define GET_SUBTARGETINFO_TARGET_DESC
#define GET_SUBTARGETINFO_CTOR
#include "MipsGenSubtargetInfo.inc"

#define OPTIONS_STRUCT_DEFS
#include "MipsOptions.inc"

bool MipsSubtarget::DspWarningPrinted = false;
bool MipsSubtarget::MSAWarningPrinted = false;
bool MipsSubtarget::VirtWarningPrinted = false;
bool MipsSubtarget::CRCWarningPrinted = false;
bool MipsSubtarget::GINVWarningPrinted = false;

void MipsSubtarget::anchor() {}

// The size of computed-goto dispatches influences register pressure and copy
// coalescing on the R5900. Share the detection across both policies.
static bool isLargeDispatch(const Function &F) {
  if (F.size() < 512)
    return false;
  for (const BasicBlock &BB : F)
    if (isa<IndirectBrInst>(BB.getTerminator()))
      return true;
  return false;
}

// Compact kernels with several basic blocks around an annotated loop can
// benefit from ILP-aware ordering without the larger live ranges typical of
// computed-goto interpreters. Keep the default for other small functions.
static bool isCompactLoopKernel(const Function &F) {
  if (F.size() < 24 || F.size() > 32)
    return false;
  unsigned LoopLatches = 0;
  unsigned NumInstructions = 0;
  for (const BasicBlock &BB : F) {
    NumInstructions += BB.size();
    LoopLatches +=
        BB.getTerminator()->getMetadata(LLVMContext::MD_loop) != nullptr;
  }
  return LoopLatches == 1 && NumInstructions >= 160;
}

// Very large computed-goto dispatch functions are unusually register-pressure
// sensitive on the R5900. Prefer the bottom-up register-pressure scheduler for
// these functions; preserve the existing source-order scheduler for smaller
// functions and for all other MIPS CPUs.
static ScheduleDAGSDNodes *
createR5900DispatchDAGScheduler(SelectionDAGISel *IS,
                                CodeGenOptLevel OptLevel) {
  const Function &F = IS->MF->getFunction();
  if (isLargeDispatch(F)) {
    LLVM_DEBUG(dbgs() << "R5900 DAG scheduler: register pressure for "
                      << F.getName() << '\n');
    return createBURRListDAGScheduler(IS, OptLevel);
  }
  if (isCompactLoopKernel(F)) {
    LLVM_DEBUG(dbgs() << "R5900 DAG scheduler: ILP for "
                      << F.getName() << '\n');
    return createILPListDAGScheduler(IS, OptLevel);
  }
  LLVM_DEBUG(dbgs() << "R5900 DAG scheduler: source for " << F.getName()
                    << '\n');
  return createSourceListDAGScheduler(IS, OptLevel);
}

RegisterScheduler::FunctionPassCtor
MipsSubtarget::getDAGScheduler(CodeGenOptLevel OptLevel) const {
  if (isR5900() && OptLevel != CodeGenOptLevel::None)
    return createR5900DispatchDAGScheduler;
  return nullptr;
}

MipsSubtarget::MipsSubtarget(const Triple &TT, StringRef CPU, StringRef FS,
                             StringRef ABIName, bool little,
                             const MipsTargetMachine &TM,
                             MaybeAlign StackAlignOverride)
    : MipsGenSubtargetInfo(TT, CPU, /*TuneCPU*/ CPU, FS),
      CLOpts(TM.getCLOpts()), MCCLOpts(TM.getMCCLOpts()),
      MipsArchVersion(MipsDefault), IsLittle(little), IsSoftFloat(false),
      IsSingleFloat(false), IsFPXX(false), NoABICalls(false), Abs2008(false),
      IsFP64bit(false), UseOddSPReg(true), IsNaN2008bit(false),
      IsGP64bit(false), HasVFPU(false), HasCnMips(false), HasCnMipsP(false),
      IsR5900(false), FixR5900(false), HasMips3_32(false), HasMips3_32r2(false),
      HasMips4_32(false), HasMips4_32r2(false), HasMips5_32r2(false),
      InMips16Mode(false), InMips16HardFloat(CLOpts.mips16_hard_float),
      InMicroMipsMode(false), HasDSP(false), HasDSPR2(false), HasDSPR3(false),
      HasMSA(false), UseTCCInDIV(false), HasSym32(false), HasEVA(false),
      DisableMadd4(false), HasMT(false), HasCRC(false), HasVirt(false),
      HasGINV(false), UseIndirectJumpsHazard(false), StrictAlign(false),
      UseCompactBranches(CLOpts.compact_branches != CompactBranchPolicy::Never),
      StackAlignOverride(StackAlignOverride), TM(TM),
      ABI(MipsABIInfo::computeTargetABI(TT, ABIName)),
      InstrInfo(
          MipsInstrInfo::create(initializeSubtargetDependencies(CPU, FS, TM))),
      FrameLowering(MipsFrameLowering::create(*this)),
      TLInfo(MipsTargetLowering::create(TM, *this)) {

  if (MipsArchVersion == MipsDefault)
    MipsArchVersion = Mips32;

  // Don't even attempt to generate code for MIPS-V. It has not
  // been tested and currently exists for the integrated assembler only.
  if (MipsArchVersion == Mips5)
    report_fatal_error("Code generation for MIPS-V is not implemented", false);

  // Check if Architecture and ABI are compatible.
  assert(((!isGP64bit() && isABI_O32()) || isGP64bit()) &&
         "Invalid  Arch & ABI pair.");

  if (hasMSA() && !isFP64bit())
    report_fatal_error("MSA requires a 64-bit FPU register file (FR=1 mode). "
                       "See -mattr=+fp64.",
                       false);

  if (isFP64bit() && !hasMips64() && hasMips32() && !hasMips32r2())
    report_fatal_error(
        "FPU with 64-bit registers is not available on MIPS32 pre revision 2. "
        "Use -mcpu=mips32r2 or greater.", false);

  if (!isABI_O32() && !useOddSPReg())
    report_fatal_error("-mattr=+nooddspreg requires the O32 ABI.", false);

  if (IsFPXX && (isABI_N32() || isABI_N64()))
    report_fatal_error("FPXX is not permitted for the N32/N64 ABI's.", false);

  if (hasMips64r6() && InMicroMipsMode)
    report_fatal_error("microMIPS64R6 is not supported", false);

  if (!isABI_O32() && InMicroMipsMode)
    report_fatal_error("microMIPS64 is not supported.", false);

  if (UseIndirectJumpsHazard) {
    if (InMicroMipsMode)
      report_fatal_error(
          "cannot combine indirect jumps with hazard barriers and microMIPS");
    if (!hasMips32r2())
      report_fatal_error(
          "indirect jumps with hazard barriers requires MIPS32R2 or later");
  }
  if (inAbs2008Mode() && hasMips32() && !hasMips32r2()) {
    report_fatal_error("IEEE 754-2008 abs.fmt is not supported for the given "
                       "architecture.",
                       false);
  }

  if (hasMips32r6()) {
    StringRef ISA = hasMips64r6() ? "MIPS64r6" : "MIPS32r6";

    assert(isFP64bit());
    assert(isNaN2008());
    assert(inAbs2008Mode());
    if (hasDSP())
      report_fatal_error(ISA + " is not compatible with the DSP ASE", false);
  }

  if (NoABICalls && TM.isPositionIndependent())
    report_fatal_error("position-independent code requires '-mabicalls'");

  if (isABI_N64() && !TM.isPositionIndependent() && !hasSym32())
    NoABICalls = true;

  // Set UseSmallSection.
  UseSmallSection = CLOpts.mgpopt;
  if (!NoABICalls && CLOpts.mgpopt) {
    errs() << "warning: cannot use small-data accesses for '-mabicalls'"
           << "\n";
    UseSmallSection = false;
  }

  if (hasDSPR2() && !DspWarningPrinted) {
    if (hasMips64() && !hasMips64r2()) {
      errs() << "warning: the 'dspr2' ASE requires MIPS64 revision 2 or "
             << "greater\n";
      DspWarningPrinted = true;
    } else if (hasMips32() && !hasMips32r2()) {
      errs() << "warning: the 'dspr2' ASE requires MIPS32 revision 2 or "
             << "greater\n";
      DspWarningPrinted = true;
    }
  } else if (hasDSP() && !DspWarningPrinted) {
    if (hasMips64() && !hasMips64r2()) {
      errs() << "warning: the 'dsp' ASE requires MIPS64 revision 2 or "
             << "greater\n";
      DspWarningPrinted = true;
    } else if (hasMips32() && !hasMips32r2()) {
      errs() << "warning: the 'dsp' ASE requires MIPS32 revision 2 or "
             << "greater\n";
      DspWarningPrinted = true;
    }
  }

  StringRef ArchName = hasMips64() ? "MIPS64" : "MIPS32";

  if (!hasMips32r5() && hasMSA() && !MSAWarningPrinted) {
    errs() << "warning: the 'msa' ASE requires " << ArchName
           << " revision 5 or greater\n";
    MSAWarningPrinted = true;
  }
  if (!hasMips32r5() && hasVirt() && !VirtWarningPrinted) {
    errs() << "warning: the 'virt' ASE requires " << ArchName
           << " revision 5 or greater\n";
    VirtWarningPrinted = true;
  }
  if (!hasMips32r6() && hasCRC() && !CRCWarningPrinted) {
    errs() << "warning: the 'crc' ASE requires " << ArchName
           << " revision 6 or greater\n";
    CRCWarningPrinted = true;
  }
  if (!hasMips32r6() && hasGINV() && !GINVWarningPrinted) {
    errs() << "warning: the 'ginv' ASE requires " << ArchName
           << " revision 6 or greater\n";
    GINVWarningPrinted = true;
  }

  TSInfo = std::make_unique<MipsSelectionDAGInfo>();

  CallLoweringInfo.reset(new MipsCallLowering(*getTargetLowering()));
  Legalizer.reset(new MipsLegalizerInfo(*this));

  auto *RBI = new MipsRegisterBankInfo(*getRegisterInfo());
  RegBankInfo.reset(RBI);
  InstSelector.reset(createMipsInstructionSelector(TM, *this, *RBI));
}

MipsSubtarget::~MipsSubtarget() = default;

bool MipsSubtarget::isPositionIndependent() const {
  return TM.isPositionIndependent();
}

/// This overrides the PostRAScheduler bit in the SchedModel for any CPU.
/// The R5900 uses the post-RA MachineScheduler instead.
bool MipsSubtarget::enablePostRAScheduler() const { return !isR5900(); }

/// The R5900 is dual-issue and in-order, use the MachineScheduler with its
/// scheduling model both before and after register allocation.
bool MipsSubtarget::enableMachineScheduler() const { return isR5900(); }

bool MipsSubtarget::enableJoinGlobalCopies(const MachineFunction &MF) const {
  // Medium-sized computed-goto interpreters have many long-lived scalar
  // values. Cross-block coalescing extends their live ranges and creates
  // expensive register-pressure interactions with the R5900's machine
  // scheduler. Preserve local coalescing and the scheduler itself.
  const Function &F = MF.getFunction();
  // Short loop kernels with many PHIs tend to create long live ranges when
  // cross-block copies are coalesced. Keep copies local to avoid extra spills.
  if (isR5900() && F.size() >= 12 && F.size() <= 20) {
    unsigned NumPhis = 0, LoopLatches = 0;
    for (const BasicBlock &BB : F) {
      for (const Instruction &I : BB)
        NumPhis += isa<PHINode>(I);
      LoopLatches +=
          BB.getTerminator()->getMetadata(LLVMContext::MD_loop) != nullptr;
    }
    if (NumPhis >= 12 && LoopLatches == 1) {
      LLVM_DEBUG(dbgs() << "R5900 coalescing: skip global copies for PHI-rich "
                        << F.getName() << '\n');
      return false;
    }
  }
  if (isR5900() && F.size() >= 64 && F.size() < 512) {
    for (const BasicBlock &BB : F) {
      if (isa<IndirectBrInst>(BB.getTerminator())) {
        LLVM_DEBUG(dbgs() << "R5900 coalescing: skip global copies for "
                          << F.getName() << '\n');
        return false;
      }
    }
  }
  return TargetSubtargetInfo::enableJoinGlobalCopies();
}

bool MipsSubtarget::enableJoinSplitEdges(const MachineFunction &MF) const {
  // Revisit critical-edge split copies first in very large computed-goto
  // functions. This reduces live-range interference and spills in the R5900
  // register allocator without changing the policy for smaller functions.
  const Function &F = MF.getFunction();
  if (isR5900() && isLargeDispatch(F)) {
    LLVM_DEBUG(dbgs() << "R5900 coalescing: prioritize split edges for "
                      << F.getName() << '\n');
    return true;
  }
  return false;
}

bool MipsSubtarget::enableR5900IssueNops(const MachineFunction &MF) const {
  // Large computed-goto dispatchers execute many short, mutually exclusive
  // opcode handlers. Inserting speculative dual-issue padding throughout those
  // handlers increases the code footprint and can outweigh the locally
  // estimated scheduling benefit. Keep instruction reordering, but omit
  // optional nop padding for this structural case.
  return !isR5900() || !isLargeDispatch(MF.getFunction());
}

bool MipsSubtarget::enableBranchFolderBlockReordering(
    const MachineFunction &MF) const {
  if (!isR5900() || !MF.getFunction().getReturnType()->isIntegerTy(8))
    return true;

  // A near-certain byte-load fast path that jumps straight to a shared
  // return can use the load in the return's delay slot if the two blocks
  // remain adjacent. The branch folder may otherwise move the shared
  // return elsewhere even though the source marks this path as likely.
  const MachineBasicBlock &Entry = MF.front();
  for (auto I = Entry.succ_begin(); I != Entry.succ_end(); ++I) {
    const MachineBasicBlock *Hot = *I;
    if (Entry.getSuccProbability(I) < BranchProbability(19, 20) ||
        Hot->succ_size() != 1)
      continue;

    const MachineBasicBlock *Ret = *Hot->succ_begin();
    if (Ret->empty() || !Ret->back().isReturn())
      continue;

    for (auto RI = Hot->rbegin(); RI != Hot->rend(); ++RI) {
      if (RI->isTerminator() || RI->isMetaInstruction())
        continue;
      if (RI->getOpcode() == Mips::LBu) {
        LLVM_DEBUG(dbgs() << "R5900 branch folder: preserve hot byte-load "
                             "return in "
                          << MF.getName() << '\n');
        return false;
      }
      break;
    }
  }
  return true;
}

bool MipsSubtarget::enablePostRAMachineScheduler() const { return isR5900(); }

/// Use alias analysis during code generation (DAGCombine and scheduling) on
/// the in-order R5900, so loads can be scheduled ahead of stores that cannot
/// alias them.
bool MipsSubtarget::useAA() const { return isR5900(); }

static cl::opt<bool>
    R5900Pipeliner("mips-r5900-pipeliner", cl::Hidden, cl::init(true),
                   cl::desc("Software pipeline the loops of the R5900"));

// Measured on the PS2: N64 To CT16 +11%, the other tests the same.
static cl::opt<bool> R5900PipelineFPU(
    "mips-r5900-pipeline-fpu", cl::Hidden, cl::init(true),
    cl::desc("Also software pipeline the FPU and MMI loops of the R5900"));

bool MipsSubtarget::enableMachinePipeliner() const {
  return isR5900() && (hasVU0() || R5900PipelineFPU) && R5900Pipeliner;
}

bool MipsSubtarget::pipelineFPULoops() const {
  return isR5900() && R5900PipelineFPU;
}

void MipsSubtarget::overridePipelinerPolicy(
    MachinePipelinerPolicy &Policy) const {
  // A VU0 operation takes 4 cycles and one COP2 issue slot, so a chain of
  // them spans several stages of a short loop.
  Policy.UseIterativeScheduler = true;
  Policy.MaxStages = 6;
  Policy.ShouldLimitRegPressure = true;
  // Unroll the kernel instead of copying values: see MipsPipelinerLoopInfo.
  Policy.UseMVE = true;
}

void MipsSubtarget::getSMSMutations(
    std::vector<std::unique_ptr<ScheduleDAGMutation>> &Mutations) const {
  // ACC chains (VMULA, VMADDA..., VMADD), divisions through Q (VDIV,
  // VWAITQ, VMULQ) and the HI/LO sums of MipsR5900LoopAcc.
  Mutations.push_back(createSingleRegChainStartMutation(
      {&Mips::VU0ACCRCRegClass, &Mips::VU0QVRegClass, &Mips::HILO128RCRegClass,
       &Mips::ACC64RegClass}));
}

void MipsSubtarget::getCriticalPathRCs(RegClassVector &CriticalPathRCs) const {
  CriticalPathRCs.clear();
  CriticalPathRCs.push_back(isGP64bit() ? &Mips::GPR64RegClass
                                        : &Mips::GPR32RegClass);
}

CodeGenOptLevel MipsSubtarget::getOptLevelToEnablePostRAScheduler() const {
  return CodeGenOptLevel::Aggressive;
}

MipsSubtarget &
MipsSubtarget::initializeSubtargetDependencies(StringRef CPU, StringRef FS,
                                               const TargetMachine &TM) {
  StringRef CPUName = MIPS_MC::selectMipsCPU(TM.getTargetTriple(), CPU);

  // Parse features string.
  ParseSubtargetFeatures(CPUName, /*TuneCPU*/ CPUName, FS);

  // O32 uses 32-bit GPRs even when the selected CPU supports a 64-bit ISA.
  if (isABI_O32())
    IsGP64bit = false;

  if (InMips16Mode && !IsSoftFloat)
    InMips16HardFloat = true;

  if (StackAlignOverride)
    stackAlignment = *StackAlignOverride;
  else if (isABI_N32() || isABI_N64())
    stackAlignment = Align(16);
  else {
    assert(isABI_O32() && "Unknown ABI for stack alignment!");
    stackAlignment = Align(8);
  }

  if ((isABI_N32() || isABI_N64()) && !isGP64bit())
    reportFatalUsageError("64-bit code requested on a subtarget that doesn't "
                          "support it!");

  return *this;
}

bool MipsSubtarget::useConstantIslands() const {
  LLVM_DEBUG(dbgs() << "use constant islands " << CLOpts.mips16_constant_islands
                    << "\n");
  return CLOpts.mips16_constant_islands;
}

Reloc::Model MipsSubtarget::getRelocationModel() const {
  return TM.getRelocationModel();
}

bool MipsSubtarget::isABI_N64() const { return getABI().IsN64(); }
bool MipsSubtarget::isABI_N32() const { return getABI().IsN32(); }
bool MipsSubtarget::isABI_O32() const { return getABI().IsO32(); }

const SelectionDAGTargetInfo *MipsSubtarget::getSelectionDAGInfo() const {
  return TSInfo.get();
}

const CallLowering *MipsSubtarget::getCallLowering() const {
  return CallLoweringInfo.get();
}

const LegalizerInfo *MipsSubtarget::getLegalizerInfo() const {
  return Legalizer.get();
}

const RegisterBankInfo *MipsSubtarget::getRegBankInfo() const {
  return RegBankInfo.get();
}

InstructionSelector *MipsSubtarget::getInstructionSelector() const {
  return InstSelector.get();
}

// Libcalls for which no helper is generated. Sorted by name for binary search.
const RTLIB::LibcallImpl MipsSubtarget::HardFloatLibCalls[34] = {
    RTLIB::impl___mips16_adddf3,        RTLIB::impl___mips16_addsf3,
    RTLIB::impl___mips16_divdf3,        RTLIB::impl___mips16_divsf3,
    RTLIB::impl___mips16_eqdf2,         RTLIB::impl___mips16_eqsf2,
    RTLIB::impl___mips16_extendsfdf2,   RTLIB::impl___mips16_fix_truncdfsi,
    RTLIB::impl___mips16_fix_truncsfsi, RTLIB::impl___mips16_floatsidf,
    RTLIB::impl___mips16_floatsisf,     RTLIB::impl___mips16_floatunsidf,
    RTLIB::impl___mips16_floatunsisf,   RTLIB::impl___mips16_gedf2,
    RTLIB::impl___mips16_gesf2,         RTLIB::impl___mips16_gtdf2,
    RTLIB::impl___mips16_gtsf2,         RTLIB::impl___mips16_ledf2,
    RTLIB::impl___mips16_lesf2,         RTLIB::impl___mips16_ltdf2,
    RTLIB::impl___mips16_ltsf2,         RTLIB::impl___mips16_muldf3,
    RTLIB::impl___mips16_mulsf3,        RTLIB::impl___mips16_nedf2,
    RTLIB::impl___mips16_nesf2,         RTLIB::impl___mips16_ret_dc,
    RTLIB::impl___mips16_ret_df,        RTLIB::impl___mips16_ret_sc,
    RTLIB::impl___mips16_ret_sf,        RTLIB::impl___mips16_subdf3,
    RTLIB::impl___mips16_subsf3,        RTLIB::impl___mips16_truncdfsf2,
    RTLIB::impl___mips16_unorddf2,      RTLIB::impl___mips16_unordsf2};

void MipsSubtarget::initLibcallLoweringInfo(LibcallLoweringInfo &Info) const {
  if (inMips16Mode() && !useSoftFloat()) {
    for (unsigned I = 0; I != std::size(HardFloatLibCalls); ++I) {
      assert((I == 0 || HardFloatLibCalls[I - 1] < HardFloatLibCalls[I]) &&
             "Array not sorted!");
      RTLIB::Libcall LC =
          RTLIB::RuntimeLibcallsInfo::getLibcallFromImpl(HardFloatLibCalls[I]);
      Info.setLibcallImpl(LC, HardFloatLibCalls[I]);
    }
  }
}
