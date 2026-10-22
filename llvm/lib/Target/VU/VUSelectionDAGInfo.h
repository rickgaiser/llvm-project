//===-- VUSelectionDAGInfo.h - VU SelectionDAG info -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_VU_VUSELECTIONDAGINFO_H
#define LLVM_LIB_TARGET_VU_VUSELECTIONDAGINFO_H

#include "llvm/CodeGen/SelectionDAGTargetInfo.h"

#define GET_SDNODE_ENUM
#include "VUGenSDNodeInfo.inc"

namespace llvm {

namespace VUISD {
/// The field operand of an ACCSUM term that uses all fields of b.
constexpr unsigned AccSumFull = 4;
} // namespace VUISD

class VUSelectionDAGInfo : public SelectionDAGGenTargetInfo {
public:
  VUSelectionDAGInfo();
  ~VUSelectionDAGInfo() override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_VU_VUSELECTIONDAGINFO_H
