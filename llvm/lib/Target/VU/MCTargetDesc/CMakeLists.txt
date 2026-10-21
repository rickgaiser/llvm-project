add_llvm_component_library(LLVMVUDesc
  VUAsmBackend.cpp
  VUELFObjectWriter.cpp
  VUInstPrinter.cpp
  VUMCAsmInfo.cpp
  VUMCCodeEmitter.cpp
  VUMCTargetDesc.cpp

  LINK_COMPONENTS
  MC
  Support
  TargetParser
  VUInfo

  ADD_TO_COMPONENT
  VU
  )
