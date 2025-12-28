add_llvm_component_group(DVPVU)

set(LLVM_TARGET_DEFINITIONS DVPVU.td)

tablegen(LLVM DVPVUGenRegisterInfo.inc -gen-register-info)
tablegen(LLVM DVPVUGenInstrInfo.inc -gen-instr-info)
tablegen(LLVM DVPVUGenSubtargetInfo.inc -gen-subtarget)
tablegen(LLVM DVPVUGenCallingConv.inc -gen-callingconv)
tablegen(LLVM DVPVUGenDAGISel.inc -gen-dag-isel)
tablegen(LLVM DVPVUGenAsmWriter.inc -gen-asm-writer)
tablegen(LLVM DVPVUGenAsmMatcher.inc -gen-asm-matcher)
tablegen(LLVM DVPVUGenDisassemblerTables.inc -gen-disassembler)
tablegen(LLVM DVPVUGenMCCodeEmitter.inc -gen-emitter)
tablegen(LLVM DVPVUGenSDNodeInfo.inc -gen-sd-node-info)

add_public_tablegen_target(DVPVUCommonTableGen)

add_subdirectory(TargetInfo)
