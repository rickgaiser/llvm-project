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

# VU0 Arithmetic Instructions - Vector Add/Subtract/Multiply

# VADD - Vector Add (all lanes)
# CHECK: vadd.xyzw       $$vf1, $$vf2, $$vf3     # encoding: [0x68,0x10,0xe3,0x4b]
vadd.xyzw $vf1, $vf2, $vf3

# VSUB - Vector Subtract
# CHECK: vsub.xyzw       $$vf4, $$vf5, $$vf6     # encoding: [0x2c,0x29,0xe6,0x4b]
vsub.xyzw $vf4, $vf5, $vf6

# VMUL - Vector Multiply
# CHECK: vmul.xyzw       $$vf7, $$vf8, $$vf9     # encoding: [0xea,0x41,0xe9,0x4b]
vmul.xyzw $vf7, $vf8, $vf9

# VMADD - Vector Multiply-Add (ACC + fs*ft -> fd)
# CHECK: vmadd.xyzw      $$vf10, $$vf11, $$vf12  # encoding: [0xa9,0x5a,0xec,0x4b]
vmadd.xyzw $vf10, $vf11, $vf12

# VMSUB - Vector Multiply-Subtract (ACC - fs*ft -> fd)
# CHECK: vmsub.xyzw      $$vf13, $$vf14, $$vf15  # encoding: [0x6d,0x73,0xef,0x4b]
vmsub.xyzw $vf13, $vf14, $vf15

# VU0 ACC-destination Instructions (write to accumulator)

# VADDA - Vector Add to ACC
# CHECK: vadda.xyzw      $$vf16, $$vf17          # encoding: [0xfc,0x85,0xf1,0x4b]
vadda.xyzw $vf16, $vf17

# VSUBA - Vector Subtract to ACC
# CHECK: vsuba.xyzw      $$vf18, $$vf19          # encoding: [0xfd,0x95,0xf3,0x4b]
vsuba.xyzw $vf18, $vf19

# VMULA - Vector Multiply to ACC
# CHECK: vmula.xyzw      $$vf20, $$vf21          # encoding: [0xfe,0xa5,0xf5,0x4b]
vmula.xyzw $vf20, $vf21

# VMADDA - Vector Multiply-Add to ACC
# CHECK: vmadda.xyzw     $$vf22, $$vf23          # encoding: [0xff,0xb5,0xf7,0x4b]
vmadda.xyzw $vf22, $vf23

# VMSUBA - Vector Multiply-Subtract from ACC
# CHECK: vmsuba.xyzw     $$vf24, $$vf25          # encoding: [0xfb,0xc5,0xf9,0x4b]
vmsuba.xyzw $vf24, $vf25
