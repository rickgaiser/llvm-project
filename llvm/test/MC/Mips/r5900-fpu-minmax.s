# RUN: llvm-mc %s -triple=mips64el-unknown-linux -mcpu=r5900 -show-encoding \
# RUN:   | FileCheck %s

# Test R5900 FPU min/max and rsqrt instructions

# CHECK: max.s $f0, $f1, $f2   # encoding: [0x28,0x08,0x02,0x46]
max.s $f0, $f1, $f2

# CHECK: min.s $f3, $f4, $f5   # encoding: [0xe9,0x20,0x05,0x46]
min.s $f3, $f4, $f5

# CHECK: rsqrt.s $f6, $f7, $f8 # encoding: [0x96,0x39,0x08,0x46]
rsqrt.s $f6, $f7, $f8
