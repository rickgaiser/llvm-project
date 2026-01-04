//===--- PS2.cpp - PS2 ToolChain Implementations ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PS2.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Options/Options.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <cstdlib>

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang;
using namespace llvm::opt;

PS2Toolchain::PS2Toolchain(const Driver &D, const llvm::Triple &Triple,
                           const ArgList &Args)
    : Generic_ELF(D, Triple, Args) {
  // Get PS2DEV directory from environment variable.
  // PS2DEV is the root of the PS2 development environment:
  //   $PS2DEV/mips64el-scei-ps2 - EE sysroot with newlib headers/libs
  //   $PS2DEV/llvm              - LLVM toolchain installation
  if (const char *PS2DevEnv = std::getenv("PS2DEV")) {
    PS2DevDir = PS2DevEnv;
  }

  // Get PS2SDK directory from environment variable.
  // PS2SDK structure:
  //   $PS2SDK/ee/include     - EE-specific headers (kernel, hardware)
  //   $PS2SDK/common/include - Common headers (shared between EE/IOP)
  //   $PS2SDK/ee/lib         - EE libraries
  if (const char *PS2SDKEnv = std::getenv("PS2SDK")) {
    PS2SDKDir = PS2SDKEnv;
  }

  // Set up library search paths.
  // 1. From sysroot if explicitly specified
  if (!D.SysRoot.empty()) {
    getFilePaths().push_back(D.SysRoot + "/lib");
  }

  // 2. From PS2DEV (newlib libraries)
  if (!PS2DevDir.empty()) {
    getFilePaths().push_back(PS2DevDir + "/mips64el-scei-ps2/lib");
  }

  // 3. From PS2SDK
  if (!PS2SDKDir.empty()) {
    getFilePaths().push_back(PS2SDKDir + "/ee/lib");
  }
}

void PS2Toolchain::addClangTargetOptions(const ArgList &DriverArgs,
                                         ArgStringList &CC1Args,
                                         Action::OffloadKind) const {
  // Prevent default system include paths from being added.
  CC1Args.push_back("-nostdsysteminc");

  // Define _EE macro for PS2 Emotion Engine.
  CC1Args.push_back("-D_EE");

  // Define __ps2sdk__ for PS2SDK compatibility.
  CC1Args.push_back("-D__ps2sdk__");
}

const char *PS2Toolchain::getDefaultLinker() const { return "ld.lld"; }

std::string PS2Toolchain::computeSysRoot() const {
  // Use explicit sysroot if provided.
  if (!getDriver().SysRoot.empty())
    return getDriver().SysRoot;

  // Otherwise use $PS2DEV/mips64el-scei-ps2 as the default sysroot.
  if (!PS2DevDir.empty())
    return PS2DevDir + "/mips64el-scei-ps2";

  return std::string();
}

void PS2Toolchain::AddClangSystemIncludeArgs(const ArgList &DriverArgs,
                                             ArgStringList &CC1Args) const {
  const Driver &D = getDriver();

  if (DriverArgs.hasArg(options::OPT_nostdinc))
    return;

  // Add Clang's resource directory include path (built-in headers like
  // stddef.h, stdint.h, stdarg.h, etc.). When clang is installed to
  // $PS2DEV/llvm, this will be $PS2DEV/llvm/lib/clang/<version>/include.
  if (!DriverArgs.hasArg(options::OPT_nobuiltininc)) {
    SmallString<128> ResourceDir(D.ResourceDir);
    llvm::sys::path::append(ResourceDir, "include");
    addSystemInclude(DriverArgs, CC1Args, ResourceDir.str());
  }

  if (DriverArgs.hasArg(options::OPT_nostdlibinc))
    return;

  // Add newlib include paths from explicit sysroot if specified.
  if (!D.SysRoot.empty()) {
    SmallString<128> SysrootInclude(D.SysRoot);
    llvm::sys::path::append(SysrootInclude, "include");
    if (llvm::sys::fs::exists(SysrootInclude))
      addExternCSystemInclude(DriverArgs, CC1Args, SysrootInclude.str());
  }

  // Add newlib include paths from PS2DEV.
  // PS2DEV structure:
  //   $PS2DEV/mips64el-scei-ps2/include - newlib headers for EE target
  if (!PS2DevDir.empty()) {
    SmallString<128> NewlibInclude(PS2DevDir);
    llvm::sys::path::append(NewlibInclude, "mips64el-scei-ps2", "include");
    if (llvm::sys::fs::exists(NewlibInclude))
      addExternCSystemInclude(DriverArgs, CC1Args, NewlibInclude.str());
  }

  // Add PS2SDK include paths if PS2SDK environment variable is set.
  // PS2SDK structure:
  //   $PS2SDK/ee/include     - EE-specific headers (kernel, hardware)
  //   $PS2SDK/common/include - Common headers (shared between EE/IOP)
  if (!PS2SDKDir.empty()) {
    SmallString<128> EEInclude(PS2SDKDir);
    llvm::sys::path::append(EEInclude, "ee", "include");
    if (llvm::sys::fs::exists(EEInclude))
      addExternCSystemInclude(DriverArgs, CC1Args, EEInclude.str());

    SmallString<128> CommonInclude(PS2SDKDir);
    llvm::sys::path::append(CommonInclude, "common", "include");
    if (llvm::sys::fs::exists(CommonInclude))
      addExternCSystemInclude(DriverArgs, CC1Args, CommonInclude.str());
  }
}
