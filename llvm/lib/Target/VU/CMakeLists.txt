add_llvm_component_group(VU)

set(LLVM_TARGET_DEFINITIONS VU.td)

tablegen(LLVM VUGenAsmMatcher.inc -gen-asm-matcher)
tablegen(LLVM VUGenAsmWriter.inc -gen-asm-writer)
tablegen(LLVM VUGenDisassemblerTables.inc -gen-disassembler)
tablegen(LLVM VUGenInstrInfo.inc -gen-instr-info)
tablegen(LLVM VUGenMCCodeEmitter.inc -gen-emitter)
tablegen(LLVM VUGenRegisterInfo.inc -gen-register-info)
tablegen(LLVM VUGenSubtargetInfo.inc -gen-subtarget)

add_public_tablegen_target(VUCommonTableGen)

add_llvm_target(VUCodeGen
  VUTargetMachine.cpp

  LINK_COMPONENTS
  Core
  MC
  Support
  Target
  VUDesc
  VUInfo

  ADD_TO_COMPONENT
  VU
  )

add_subdirectory(AsmParser)
add_subdirectory(Disassembler)
add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
