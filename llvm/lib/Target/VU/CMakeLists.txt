add_llvm_component_group(VU)

set(LLVM_TARGET_DEFINITIONS VU.td)

tablegen(LLVM VUGenAsmMatcher.inc -gen-asm-matcher)
tablegen(LLVM VUGenAsmWriter.inc -gen-asm-writer)
tablegen(LLVM VUGenDAGISel.inc -gen-dag-isel)
tablegen(LLVM VUGenDFAPacketizer.inc -gen-dfa-packetizer)
tablegen(LLVM VUGenDisassemblerTables.inc -gen-disassembler)
tablegen(LLVM VUGenInstrInfo.inc -gen-instr-info)
tablegen(LLVM VUGenMCCodeEmitter.inc -gen-emitter)
tablegen(LLVM VUGenRegisterInfo.inc -gen-register-info)
tablegen(LLVM VUGenSDNodeInfo.inc -gen-sd-node-info)
tablegen(LLVM VUGenSubtargetInfo.inc -gen-subtarget)

add_public_tablegen_target(VUCommonTableGen)

add_llvm_target(VUCodeGen
  VUAsmPrinter.cpp
  VUDelaySlotFiller.cpp
  VUFoldInsertField.cpp
  VUFoldQ.cpp
  VUFrameLowering.cpp
  VUHazards.cpp
  VUInstrInfo.cpp
  VUISelDAGToDAG.cpp
  VUISelLowering.cpp
  VULoopCopies.cpp
  VULoopScheduler.cpp
  VUPackConstants.cpp
  VUPacketizer.cpp
  VURegisterInfo.cpp
  VUSelectionDAGInfo.cpp
  VUSubtarget.cpp
  VUTargetMachine.cpp
  VUTargetObjectFile.cpp
  VUWords.cpp

  LINK_COMPONENTS
  AsmPrinter
  CodeGen
  CodeGenTypes
  Core
  MC
  SelectionDAG
  Support
  Target
  TargetParser
  VUDesc
  VUInfo

  ADD_TO_COMPONENT
  VU
  )

add_subdirectory(AsmParser)
add_subdirectory(Disassembler)
add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
