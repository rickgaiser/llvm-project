# RUN: llvm-mc %s -triple=mips64el-unknown-linux -mcpu=r5900 -mattr=+vu0 -show-encoding \
# RUN:   | FileCheck %s

# Test R5900 VU0 (COP2) instructions - VF register load/store

# LQC2 - Load Quadword to COP2 (128-bit load to VF register)
# CHECK: lqc2    $$vf0, 0($4)            # encoding: [0x00,0x00,0x80,0xd8]
lqc2 $vf0, 0($4)

# CHECK: lqc2    $$vf1, 0($4)            # encoding: [0x00,0x00,0x81,0xd8]
lqc2 $vf1, 0($4)

# CHECK: lqc2    $$vf12, 256($16)        # encoding: [0x00,0x01,0x0c,0xda]
lqc2 $vf12, 256($16)

# CHECK: lqc2    $$vf31, -128($17)       # encoding: [0x80,0xff,0x3f,0xda]
lqc2 $vf31, -128($17)

# SQC2 - Store Quadword from COP2 (VF register to 128-bit memory)
# CHECK: sqc2    $$vf0, 0($5)            # encoding: [0x00,0x00,0xa0,0xf8]
sqc2 $vf0, 0($5)

# CHECK: sqc2    $$vf1, 16($5)           # encoding: [0x10,0x00,0xa1,0xf8]
sqc2 $vf1, 16($5)

# CHECK: sqc2    $$vf20, -128($17)       # encoding: [0x80,0xff,0x34,0xfa]
sqc2 $vf20, -128($17)

# CHECK: sqc2    $$vf31, 32767($zero)    # encoding: [0xff,0x7f,0x1f,0xf8]
sqc2 $vf31, 32767($zero)
