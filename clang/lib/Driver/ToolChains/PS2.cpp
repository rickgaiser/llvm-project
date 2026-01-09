//===--- PS2.cpp - PS2 ToolChain Implementations ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PS2.h"
#include "clang/Driver/CommonArgs.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/InputInfo.h"
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

Tool *PS2Toolchain::buildLinker() const {
  return new tools::ps2::Linker(*this);
}

void tools::ps2::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                                      const InputInfo &Output,
                                      const InputInfoList &Inputs,
                                      const ArgList &Args,
                                      const char *LinkingOutput) const {
  const auto &TC = static_cast<const toolchains::PS2Toolchain &>(getToolChain());
  const Driver &D = TC.getDriver();

  ArgStringList CmdArgs;

  // Linker emulation mode for MIPS N32 ABI little-endian.
  CmdArgs.push_back("-m");
  CmdArgs.push_back("elf32ltsmipn32");

  // EH frame header for exception handling.
  CmdArgs.push_back("--eh-frame-hdr");

  // Add sysroot if specified.
  if (!D.SysRoot.empty()) {
    CmdArgs.push_back(Args.MakeArgString("--sysroot=" + D.SysRoot));
  }

  // Check if we need to link startup files.
  bool NeedCRTs =
      !Args.hasArg(options::OPT_nostdlib, options::OPT_nostartfiles);

  // Link crt0.o as the startup file.
  if (NeedCRTs && !Args.hasArg(options::OPT_r)) {
    CmdArgs.push_back(Args.MakeArgString(TC.GetFilePath("crt0.o")));
  }

  // Use default linker script if user didn't specify one.
  if (!Args.hasArg(options::OPT_T) && !Args.hasArg(options::OPT_r)) {
    CmdArgs.push_back("-T");
    CmdArgs.push_back(Args.MakeArgString(TC.GetFilePath("linkfile")));
  }

  // Add user-specified linker script and other options.
  Args.addAllArgs(CmdArgs, {options::OPT_T_Group, options::OPT_L, options::OPT_u,
                            options::OPT_s, options::OPT_t, options::OPT_r});

  // Add library search paths.
  TC.AddFilePathLibArgs(Args, CmdArgs);

  // Add user inputs (object files, libraries).
  AddLinkerInputs(TC, Inputs, Args, CmdArgs, JA);

  // Link runtime libraries.
  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nodefaultlibs)) {
    // Add compiler-rt builtins.
    AddRunTimeLibs(TC, D, CmdArgs, Args);

    // Link libc and PS2SDK libraries if not disabled.
    if (!Args.hasArg(options::OPT_nolibc)) {
      CmdArgs.push_back("-lc");
      CmdArgs.push_back("-lcglue");
      CmdArgs.push_back("-lkernel");
    }
  }

  // Output file.
  CmdArgs.push_back("-o");
  CmdArgs.push_back(Output.getFilename());

  // Get the linker path.
  const char *Exec = Args.MakeArgString(TC.GetLinkerPath());

  C.addCommand(std::make_unique<Command>(
      JA, *this, ResponseFileSupport::AtFileCurCP(), Exec, CmdArgs, Inputs,
      Output));
}

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
