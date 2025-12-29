//===-- DVPVUSelectionDAGInfo.h - DVPVU SelectionDAG Info -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the DVPVU subclass for SelectionDAGTargetInfo.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_DVPVU_DVPVUSELECTIONDAGINFO_H
#define LLVM_LIB_TARGET_DVPVU_DVPVUSELECTIONDAGINFO_H

#include "llvm/CodeGen/SelectionDAGTargetInfo.h"

namespace llvm {

class DVPVUSelectionDAGInfo : public SelectionDAGTargetInfo {
public:
  DVPVUSelectionDAGInfo() = default;
  ~DVPVUSelectionDAGInfo() override = default;
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_DVPVU_DVPVUSELECTIONDAGINFO_H
