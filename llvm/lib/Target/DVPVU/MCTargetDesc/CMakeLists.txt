add_llvm_component_library(LLVMDVPVUDesc
  DVPVUAsmBackend.cpp
  DVPVUELFObjectWriter.cpp
  DVPVUInstPrinter.cpp
  DVPVUMCAsmInfo.cpp
  DVPVUMCCodeEmitter.cpp
  DVPVUMCTargetDesc.cpp

  LINK_COMPONENTS
  DVPVUInfo
  MC
  MCDisassembler
  Support
  TargetParser

  ADD_TO_COMPONENT
  DVPVU
)
