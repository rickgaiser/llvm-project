# RUN: llvm-mc %s -triple=mips64el-unknown-linux -mcpu=r5900 -show-encoding \
# RUN:   | FileCheck %s

# Test R5900 FPU accumulator instructions

# ACC-writing instructions (ACC = result)
# CHECK: adda.s  $f1, $f2      # encoding: [0x18,0x08,0x02,0x46]
adda.s  $f1, $f2

# CHECK: suba.s  $f3, $f4      # encoding: [0x19,0x18,0x04,0x46]
suba.s  $f3, $f4

# CHECK: mula.s  $f5, $f6      # encoding: [0x1a,0x28,0x06,0x46]
mula.s  $f5, $f6

# ACC read+write instructions (ACC = ACC op result)
# CHECK: madda.s $f7, $f8      # encoding: [0x1e,0x38,0x08,0x46]
madda.s $f7, $f8

# CHECK: msuba.s $f9, $f10     # encoding: [0x1f,0x48,0x0a,0x46]
msuba.s $f9, $f10

# ACC-reading instructions (fd = ACC op result)
# CHECK: madd.s  $f0, $f1, $f2 # encoding: [0x1c,0x08,0x02,0x46]
madd.s  $f0, $f1, $f2

# CHECK: msub.s  $f3, $f4, $f5 # encoding: [0xdd,0x20,0x05,0x46]
msub.s  $f3, $f4, $f5
