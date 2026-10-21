add_llvm_component_library(LLVMVUDisassembler
  VUDisassembler.cpp

  LINK_COMPONENTS
  MC
  MCDisassembler
  Support
  VUDesc
  VUInfo

  ADD_TO_COMPONENT
  VU
  )
