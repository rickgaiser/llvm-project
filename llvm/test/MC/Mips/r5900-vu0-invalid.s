# RUN: not llvm-mc %s -triple=mips64el-unknown-linux -mcpu=r5900 -mattr=+vu0 \
# RUN:   2>&1 | FileCheck %s
# RUN: not llvm-mc %s -triple=mips64el-unknown-linux -mcpu=r5900 \
# RUN:   --defsym NOVU0=1 2>&1 | FileCheck %s --check-prefix=NOVU0

# VCALLMS takes a byte address of micro memory: a multiple of 8, below 2^18.
# CHECK: :[[@LINE+1]]:{{[0-9]+}}: error: expected both 18-bit unsigned immediate and multiple of 8
        vcallms 4
# CHECK: :[[@LINE+1]]:{{[0-9]+}}: error: expected both 18-bit unsigned immediate and multiple of 8
        vcallms 0x40000
# CHECK: :[[@LINE+1]]:{{[0-9]+}}: error: expected both 18-bit unsigned immediate and multiple of 8
        vcallms -8
# CHECK: :[[@LINE+1]]:{{[0-9]+}}: error: invalid operand for instruction
        cfc2 $8, $vi32
# A VI register is no GPR.
# CHECK: :[[@LINE+1]]:{{[0-9]+}}: error: expected 32-bit signed immediate
        addu $2, $vi3, $4

# Without VU0 there are no VI registers and no micro program calls.
# NOVU0: :[[@LINE+1]]:{{[0-9]+}}: error: invalid operand for instruction
        cfc2 $8, $vi29
# NOVU0: :[[@LINE+1]]:{{[0-9]+}}: error: instruction requires a CPU feature not currently enabled
        vcallms 0
