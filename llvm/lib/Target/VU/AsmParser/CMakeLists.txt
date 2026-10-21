add_llvm_component_library(LLVMVUAsmParser
  VUAsmParser.cpp

  LINK_COMPONENTS
  MC
  MCParser
  Support
  VUDesc
  VUInfo

  ADD_TO_COMPONENT
  VU
  )
