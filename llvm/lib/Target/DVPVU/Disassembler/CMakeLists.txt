add_llvm_component_library(LLVMDVPVUDisassembler
  DVPVUDisassembler.cpp

  LINK_COMPONENTS
  DVPVUDesc
  DVPVUInfo
  MC
  MCDisassembler
  Support

  ADD_TO_COMPONENT
  DVPVU
)
