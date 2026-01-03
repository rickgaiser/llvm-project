# PS2 Emotion Engine (R5900) Toolchain
#
# This cache file builds a complete LLVM toolchain for the PlayStation 2
# Emotion Engine CPU (MIPS R5900).
#
# Usage:
#   cmake -G Ninja -C clang/cmake/caches/PS2EE.cmake \
#     -DCMAKE_INSTALL_PREFIX=$PS2DEV \
#     ../llvm
#   ninja distribution
#   ninja install-distribution
#
# The resulting toolchain can be used with:
#   clang --target=mips64el-scei-ps2 -c file.c

set(LLVM_TARGETS_TO_BUILD "Mips" CACHE STRING "")
set(LLVM_ENABLE_PROJECTS "clang;lld" CACHE STRING "")
set(LLVM_ENABLE_RUNTIMES "compiler-rt" CACHE STRING "")

set(LLVM_DEFAULT_TARGET_TRIPLE "mips64el-scei-ps2" CACHE STRING "")

# Builtins for PS2 EE (R5900)
set(LLVM_BUILTIN_TARGETS "mips64el-scei-ps2" CACHE STRING "")

set(BUILTINS_mips64el-scei-ps2_CMAKE_SYSTEM_NAME Generic CACHE STRING "")
set(BUILTINS_mips64el-scei-ps2_CMAKE_C_FLAGS "-ffreestanding" CACHE STRING "")
set(BUILTINS_mips64el-scei-ps2_CMAKE_CXX_FLAGS "-ffreestanding" CACHE STRING "")
set(BUILTINS_mips64el-scei-ps2_CMAKE_ASM_FLAGS "" CACHE STRING "")
set(BUILTINS_mips64el-scei-ps2_COMPILER_RT_BAREMETAL_BUILD ON CACHE BOOL "")
set(BUILTINS_mips64el-scei-ps2_COMPILER_RT_BUILD_CRT ON CACHE BOOL "")
set(BUILTINS_mips64el-scei-ps2_COMPILER_RT_OS_DIR "ps2" CACHE STRING "")

# Disable components not needed for bare-metal
set(COMPILER_RT_BUILD_SANITIZERS OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_XRAY OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_LIBFUZZER OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_PROFILE OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_MEMPROF OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_ORC OFF CACHE BOOL "")

set(LLVM_INSTALL_TOOLCHAIN_ONLY ON CACHE BOOL "")

set(LLVM_TOOLCHAIN_TOOLS
  llc
  llvm-ar
  llvm-as
  llvm-cxxfilt
  llvm-dis
  llvm-nm
  llvm-objcopy
  llvm-objdump
  llvm-ranlib
  llvm-readelf
  llvm-readobj
  llvm-size
  llvm-strings
  llvm-strip
  llvm-symbolizer
  opt
  CACHE STRING "")

set(LLVM_DISTRIBUTION_COMPONENTS
  clang
  lld
  clang-resource-headers
  builtins-mips64el-scei-ps2
  ${LLVM_TOOLCHAIN_TOOLS}
  CACHE STRING "")
