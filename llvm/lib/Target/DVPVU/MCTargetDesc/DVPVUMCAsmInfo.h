//===-- DVPVUMCAsmInfo.h - DVPVU asm properties -------------*- C++ -*-----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the declaration of the DVPVUMCAsmInfo class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_DVPVU_MCTARGETDESC_DVPVUMCASMINFO_H
#define LLVM_LIB_TARGET_DVPVU_MCTARGETDESC_DVPVUMCASMINFO_H

#include "llvm/MC/MCAsmInfoELF.h"

namespace llvm {
class Triple;

class DVPVUMCAsmInfo : public MCAsmInfoELF {
  void anchor() override;

public:
  explicit DVPVUMCAsmInfo(const Triple &TheTriple,
                          const MCTargetOptions &Options);
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_DVPVU_MCTARGETDESC_DVPVUMCASMINFO_H
