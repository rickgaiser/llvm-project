# RUN: llvm-mc -triple=dvpvu -show-encoding %s | FileCheck %s
# Test Load/Store (Lower) instructions
# NOTE: The memory operand syntax lq.xyzw vf1, 0(vi2) is not yet supported.
# This test focuses on the move variants which work.

# XFAIL: *
# TODO: Fix memory instruction parsing

# Load Quadword
lq.xyzw     vf1, 0(vi2)
# CHECK: lq.xyzw vf1, 0(vi2)
