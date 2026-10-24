//===- SelectionDAGSums.cpp - The terms of sums in a SelectionDAG ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/SelectionDAGSums.h"

using namespace llvm;

static void collect(SDValue V, bool Neg,
                    function_ref<std::optional<bool>(SDValue)> GetNode,
                    bool Flatten, SumTerms &S) {
  std::optional<bool> Sub = GetNode(V);
  if (!Sub) {
    S.Terms.push_back({V, Neg});
    return;
  }
  SDNode *N = V.getNode();
  S.Nodes.push_back(N);
  S.Reassoc &= N->getFlags().hasAllowReassociation();
  SDValue L = N->getOperand(0), R = N->getOperand(1);
  bool LNode = GetNode(L).has_value(), RNode = GetNode(R).has_value();
  // The sum so far first (an add is commutative). Without flattening, the
  // second operand is a term.
  if (!LNode && RNode && !*Sub) {
    std::swap(L, R);
    std::swap(LNode, RNode);
  }
  if (Flatten && RNode)
    S.Linear = false;
  collect(L, Neg, GetNode, Flatten, S);
  if (Flatten)
    collect(R, Neg != *Sub, GetNode, Flatten, S);
  else
    S.Terms.push_back({R, Neg != *Sub});
}

void llvm::collectSumTerms(SDValue Root,
                           function_ref<std::optional<bool>(SDValue)> GetNode,
                           bool Flatten, SumTerms &S) {
  collect(Root, false, GetNode, Flatten, S);
}
