# RUN: llvm-mc -triple=dvpvu -show-encoding %s | FileCheck %s
# Test FMAC (Upper) instructions

# Vector Add
add.xyzw    vf1, vf2, vf3
# CHECK: add.xyzw vf1, vf2, vf3

add.xyz     vf4, vf5, vf6
# CHECK: add.xyz vf4, vf5, vf6

add.x       vf7, vf8, vf9
# CHECK: add.x vf7, vf8, vf9

# Vector Subtract
sub.xyzw    vf10, vf11, vf12
# CHECK: sub.xyzw vf10, vf11, vf12

# Vector Multiply
mul.xyzw    vf13, vf14, vf15
# CHECK: mul.xyzw vf13, vf14, vf15

# Multiply-Add
madd.xyzw   vf16, vf17, vf18
# CHECK: madd.xyzw vf16, vf17, vf18

# Multiply-Subtract
msub.xyzw   vf19, vf20, vf21
# CHECK: msub.xyzw vf19, vf20, vf21

# Max
max.xyzw    vf22, vf23, vf24
# CHECK: max.xyzw vf22, vf23, vf24

# Mini
mini.xyzw   vf25, vf26, vf27
# CHECK: mini.xyzw vf25, vf26, vf27

# Abs
abs.xyzw    vf28, vf29
# CHECK: abs.xyzw vf28, vf29

# NOP
nop
# CHECK: nop
