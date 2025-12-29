add_llvm_component_library(LLVMDVPVUAsmParser
  DVPVUAsmParser.cpp

  LINK_COMPONENTS
  DVPVUDesc
  DVPVUInfo
  MC
  MCParser
  Support
  TargetParser

  ADD_TO_COMPONENT
  DVPVU
)
