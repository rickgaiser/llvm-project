add_llvm_component_library(LLVMDVPVUInfo
  DVPVUTargetInfo.cpp

  LINK_COMPONENTS
  MC
  Support

  ADD_TO_COMPONENT
  DVPVU
  )
