add_llvm_component_library(LLVMVUInfo
  VUTargetInfo.cpp

  LINK_COMPONENTS
  MC
  Support

  ADD_TO_COMPONENT
  VU
  )
