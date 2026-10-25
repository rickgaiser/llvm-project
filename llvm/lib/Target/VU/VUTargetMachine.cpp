//===-- VUTargetMachine.cpp - Define TargetMachine for VU -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Code generation for VU micro programs. Loops are software pipelined, then
// instructions are scheduled before and after register allocation and paired
// into micro instruction words.
//
//===----------------------------------------------------------------------===//

#include "VUTargetMachine.h"
#include "TargetInfo/VUTargetInfo.h"
#include "VU.h"
#include "VUTargetObjectFile.h"
#include "VUTargetTransformInfo.h"
#include "llvm/CodeGen/MachineScheduler.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/SingleRegClassMutations.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/Function.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeVUTarget() {
  RegisterTargetMachine<VUTargetMachine> X(getTheVUTarget());
  PassRegistry &PR = *PassRegistry::getPassRegistry();
  initializeVUAsmPrinterPass(PR);
  initializeVUDAGToDAGISelLegacyPass(PR);
  initializeVUDelaySlotFillerPass(PR);
  initializeVUPacketizerPass(PR);
  initializeVUFoldQPass(PR);
  initializeVUPackConstantsPass(PR);
  initializeVUFoldQCopiesPass(PR);
  initializeVUFoldInsertFieldPass(PR);
  initializeVUHazardsPass(PR);
  initializeVULoopCopiesPass(PR);
  initializeVULoopLayoutPass(PR);
  initializeVULoopSchedulerPass(PR);
  initializeVULowerMemoryPass(PR);
  initializeVUEntryLoweringLegacyPass(PR);
  initializeVUHLSLAdapterLegacyPass(PR);
  initializeVUNarrowIntPass(PR);
  initializeVUSinkFieldsPass(PR);
}

VUTargetMachine::VUTargetMachine(const Target &T, const Triple &TT,
                                 StringRef CPU, StringRef FS,
                                 const TargetOptions &Options,
                                 std::optional<Reloc::Model> RM,
                                 std::optional<CodeModel::Model> CM,
                                 CodeGenOptLevel OL, bool JIT)
    : CodeGenTargetMachineImpl(T, TT, CPU, FS, Options,
                               RM.value_or(Reloc::Static),
                               getEffectiveCodeModel(CM, CodeModel::Small), OL),
      TLOF(std::make_unique<VUTargetObjectFile>()) {
  initAsmInfo();
}

VUTargetMachine::~VUTargetMachine() = default;

namespace {
/// The scheduler does not see the next iteration of a loop. A register that
/// is read at the top of a single block loop and written at the bottom gets
/// a latency to the end of the block, so that it is written early enough not
/// to stall the next iteration.
class LoopCarriedLatency : public ScheduleDAGMutation {
public:
  void apply(ScheduleDAGInstrs *DAG) override {
    if (DAG->SUnits.empty())
      return;
    MachineBasicBlock *MBB = DAG->SUnits.front().getInstr()->getParent();
    if (!MBB->isSuccessor(MBB) || DAG->end() != MBB->getFirstTerminator())
      return;

    // The first read of each register that is read before it is written,
    // and the number of instructions before it.
    struct Read {
      const MachineOperand *MO;
      unsigned Index;
    };
    DenseMap<Register, Read> FirstRead;
    SmallSet<Register, 16> Written;
    unsigned Index = 0;
    for (const MachineInstr &MI : *MBB) {
      if (MI.isMetaInstruction())
        continue;
      for (const MachineOperand &MO : MI.all_uses())
        if (MO.getReg() && !Written.count(MO.getReg()))
          FirstRead.try_emplace(MO.getReg(), Read{&MO, Index});
      for (const MachineOperand &MO : MI.all_defs())
        Written.insert(MO.getReg());
      ++Index;
    }

    // The last definition of each of these registers.
    DenseMap<Register, std::pair<SUnit *, unsigned>> LastDef;
    for (SUnit &SU : DAG->SUnits)
      for (const MachineOperand &MO : SU.getInstr()->all_defs())
        if (FirstRead.count(MO.getReg()))
          LastDef[MO.getReg()] = {&SU, MO.getOperandNo()};

    for (auto [Reg, Def] : LastDef) {
      auto [SU, OpIdx] = Def;
      const Read &R = FirstRead[Reg];
      // The branch usually shares the last word, so the read is behind the
      // delay slot and about two instructions per word.
      int Latency =
          DAG->getSchedModel()->computeOperandLatency(
              SU->getInstr(), OpIdx, R.MO->getParent(), R.MO->getOperandNo()) -
          1 - R.Index / 2;
      if (Latency <= 0)
        continue;
      SDep Dep(SU, SDep::Artificial);
      Dep.setLatency(Latency);
      DAG->ExitSU.addPred(Dep);
    }
  }
};
} // namespace

ScheduleDAGInstrs *
VUTargetMachine::createMachineScheduler(MachineSchedContext *C) const {
  ScheduleDAGMILive *DAG = createSchedLive(C);
  // There is only one ACC and one Q: two values of one of them that are
  // live at the same time cost a copy to a VF register and back.
  DAG->addMutation(createSingleRegClassOrderMutation());
  return DAG;
}

ScheduleDAGInstrs *
VUTargetMachine::createPostMachineScheduler(MachineSchedContext *C) const {
  ScheduleDAGMI *DAG = createSchedPostRA(C);
  DAG->addMutation(std::make_unique<LoopCarriedLatency>());
  return DAG;
}

const VUSubtarget *VUTargetMachine::getSubtargetImpl(const Function &F) const {
  Attribute CPUAttr = F.getFnAttribute("target-cpu");
  Attribute FSAttr = F.getFnAttribute("target-features");
  std::string CPU =
      CPUAttr.isValid() ? CPUAttr.getValueAsString().str() : TargetCPU;
  std::string FS =
      FSAttr.isValid() ? FSAttr.getValueAsString().str() : TargetFS;

  auto &I = SubtargetMap[CPU + FS];
  if (!I)
    I = std::make_unique<VUSubtarget>(TargetTriple, CPU, FS, *this);
  return I.get();
}

namespace {
class VUPassConfig : public TargetPassConfig {
public:
  VUPassConfig(VUTargetMachine &TM, PassManagerBase &PM)
      : TargetPassConfig(TM, PM) {
    substitutePass(&PostRASchedulerID, &PostMachineSchedulerID);
    // A merged tail saves a few words but costs a branch and its delay slot,
    // and it can split a loop kernel.
    setEnableTailMerge(false);
  }

  VUTargetMachine &getVUTargetMachine() const {
    return getTM<VUTargetMachine>();
  }

  void addIRPasses() override {
    // Before loop strength reduction, which would make pointer induction
    // variables of the addresses.
    addPass(createVUHLSLAdapterPass());
    addPass(createVUEntryLoweringPass());
    addPass(createVUNarrowIntPass());
    addPass(createVULowerMemoryPass());
    // Reductions are expanded by VUSinkFields, before ExpandReductions.
    addPass(createVUSinkFieldsPass());
    TargetPassConfig::addIRPasses();
    // After LICM, next to instruction selection.
    addPass(createVUSinkFieldsPass());
  }

  bool addInstSelector() override {
    addPass(createVUISelDag(getVUTargetMachine(), getOptLevel()));
    return false;
  }

  void addPreRegAlloc() override {
    if (getOptLevel() != CodeGenOptLevel::None) {
      addPass(createVUPackConstantsPass());
      addPass(createVUFoldQPass());
      addPass(&MachinePipelinerID);
      addPass(createVULoopCopiesPass());
    }
  }

  void addPreSched2() override {
    if (getOptLevel() != CodeGenOptLevel::None) {
      addPass(createVUFoldQCopiesPass());
      addPass(createVUFoldInsertFieldPass());
    }
  }

  void addPreEmitPass() override {
    addPass(createVULoopLayoutPass());
    if (getOptLevel() != CodeGenOptLevel::None)
      addPass(createVUPacketizerPass());
    addPass(createVUDelaySlotFillerPass());
    if (getOptLevel() != CodeGenOptLevel::None)
      addPass(createVULoopSchedulerPass());
    addPass(createVUHazardsPass());
  }
};
} // namespace

TargetTransformInfo
VUTargetMachine::getTargetTransformInfo(const Function &F) const {
  return TargetTransformInfo(std::make_unique<VUTTIImpl>(this, F));
}

void VUTargetMachine::registerPassBuilderCallbacks(PassBuilder &PB) {
#define GET_PASS_REGISTRY "VUPassRegistry.def"
#include "llvm/Passes/TargetPassRegistry.inc"

  // Entry functions become micro programs first, so that the optimizations
  // see the loop over the items (LICM of uniforms, for example).
  PB.registerPipelineStartEPCallback(
      [](ModulePassManager &MPM, OptimizationLevel) {
        MPM.addPass(VUHLSLAdapterPass());
        MPM.addPass(VUEntryLoweringPass());
      });
}

TargetPassConfig *VUTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new VUPassConfig(*this, PM);
}
