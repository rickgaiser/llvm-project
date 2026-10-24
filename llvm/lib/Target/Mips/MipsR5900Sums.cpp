//===- MipsR5900Sums.cpp - The terms of sums in machine code --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MipsR5900Sums.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"

using namespace llvm;

static void
collect(Register Reg, bool Neg, const MachineRegisterInfo &MRI,
        function_ref<std::optional<MachineSumNode>(Register)> GetNode,
        function_ref<bool(Register)> IsSumSoFar, MachineSumTerms &S) {
  std::optional<MachineSumNode> N = GetNode(Reg);
  if (!N) {
    S.Terms.push_back({Reg, Neg});
    return;
  }
  MachineInstr *MI = MRI.getVRegDef(Reg);
  S.Nodes.push_back(MI);
  S.Reassoc &= MI->getFlag(MachineInstr::FmReassoc);
  auto IsSum = [&](Register R) {
    return GetNode(R).has_value() || IsSumSoFar(R);
  };
  Register L = N->L, R = N->R;
  bool LSum = IsSum(L), RSum = IsSum(R);
  if (!LSum && RSum) {
    // t - (sum) negates the sum.
    if (N->Sub)
      S.Linear = false;
    else
      std::swap(L, R);
  } else if (LSum && RSum) {
    S.Linear = false;
  }
  collect(L, Neg, MRI, GetNode, IsSumSoFar, S);
  collect(R, Neg != N->Sub, MRI, GetNode, IsSumSoFar, S);
}

void llvm::collectMachineSumTerms(
    Register Root, const MachineRegisterInfo &MRI,
    function_ref<std::optional<MachineSumNode>(Register)> GetNode,
    function_ref<bool(Register)> IsSumSoFar, MachineSumTerms &S) {
  collect(Root, false, MRI, GetNode, IsSumSoFar, S);
}
