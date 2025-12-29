# RUN: llvm-mc -triple=dvpvu -show-encoding %s | FileCheck %s
# Test Move (Lower) instructions

# Vector Move
move.xyzw   vf1, vf2
# CHECK: move.xyzw vf1, vf2

move.xyz    vf3, vf4
# CHECK: move.xyz vf3, vf4

# Rotate 32 bits
mr32.xyzw   vf5, vf6
# CHECK: mr32.xyzw vf5, vf6

# Move from Integer Register
mfir.xyzw   vf7, vi1
# CHECK: mfir.xyzw vf7, vi1

# Move to Integer Register (from x component)
mtir        vi2, vf8x
# CHECK: mtir vi2, vf8x
