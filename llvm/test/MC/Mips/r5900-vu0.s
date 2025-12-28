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

# VU0 Broadcast Instructions - multiply each lane by a single broadcast component

# VMULbc - Vector Multiply with broadcast (all 4 bc variants)
# CHECK: vmulx.xyzw      $$vf1, $$vf2, $$vf3     # encoding: [0x58,0x10,0xe3,0x4b]
vmulx.xyzw $vf1, $vf2, $vf3
# CHECK: vmuly.xyzw      $$vf4, $$vf5, $$vf6     # encoding: [0x19,0x29,0xe6,0x4b]
vmuly.xyzw $vf4, $vf5, $vf6
# CHECK: vmulz.xyzw      $$vf7, $$vf8, $$vf9     # encoding: [0xda,0x41,0xe9,0x4b]
vmulz.xyzw $vf7, $vf8, $vf9
# CHECK: vmulw.xyzw      $$vf10, $$vf11, $$vf12  # encoding: [0x9b,0x5a,0xec,0x4b]
vmulw.xyzw $vf10, $vf11, $vf12

# VADDbc - Vector Add with broadcast
# CHECK: vaddx.xyzw      $$vf1, $$vf2, $$vf3     # encoding: [0x40,0x10,0xe3,0x4b]
vaddx.xyzw $vf1, $vf2, $vf3
# CHECK: vaddy.xyzw      $$vf4, $$vf5, $$vf6     # encoding: [0x01,0x29,0xe6,0x4b]
vaddy.xyzw $vf4, $vf5, $vf6
# CHECK: vaddz.xyzw      $$vf7, $$vf8, $$vf9     # encoding: [0xc2,0x41,0xe9,0x4b]
vaddz.xyzw $vf7, $vf8, $vf9
# CHECK: vaddw.xyzw      $$vf10, $$vf11, $$vf12  # encoding: [0x83,0x5a,0xec,0x4b]
vaddw.xyzw $vf10, $vf11, $vf12

# VSUBbc - Vector Subtract with broadcast
# CHECK: vsubx.xyzw      $$vf13, $$vf14, $$vf15  # encoding: [0x44,0x73,0xef,0x4b]
vsubx.xyzw $vf13, $vf14, $vf15
# CHECK: vsuby.xyzw      $$vf16, $$vf17, $$vf18  # encoding: [0x05,0x8c,0xf2,0x4b]
vsuby.xyzw $vf16, $vf17, $vf18

# VMADDbc - Vector Multiply-Add with broadcast
# CHECK: vmaddx.xyzw     $$vf1, $$vf2, $$vf3     # encoding: [0x48,0x10,0xe3,0x4b]
vmaddx.xyzw $vf1, $vf2, $vf3
# CHECK: vmaddy.xyzw     $$vf4, $$vf5, $$vf6     # encoding: [0x09,0x29,0xe6,0x4b]
vmaddy.xyzw $vf4, $vf5, $vf6
# CHECK: vmaddz.xyzw     $$vf7, $$vf8, $$vf9     # encoding: [0xca,0x41,0xe9,0x4b]
vmaddz.xyzw $vf7, $vf8, $vf9
# CHECK: vmaddw.xyzw     $$vf10, $$vf11, $$vf12  # encoding: [0x8b,0x5a,0xec,0x4b]
vmaddw.xyzw $vf10, $vf11, $vf12

# VMSUBbc - Vector Multiply-Subtract with broadcast
# CHECK: vmsubx.xyzw     $$vf13, $$vf14, $$vf15  # encoding: [0x4c,0x73,0xef,0x4b]
vmsubx.xyzw $vf13, $vf14, $vf15
# CHECK: vmsuby.xyzw     $$vf16, $$vf17, $$vf18  # encoding: [0x0d,0x8c,0xf2,0x4b]
vmsuby.xyzw $vf16, $vf17, $vf18

# VMULAbc - Vector Multiply to ACC with broadcast
# CHECK: vmulax.xyzw     $$vf19, $$vf20          # encoding: [0xbc,0x99,0xf4,0x4b]
vmulax.xyzw $vf19, $vf20
# CHECK: vmulay.xyzw     $$vf21, $$vf22          # encoding: [0xbd,0xa9,0xf6,0x4b]
vmulay.xyzw $vf21, $vf22
# CHECK: vmulaz.xyzw     $$vf23, $$vf24          # encoding: [0xbe,0xb9,0xf8,0x4b]
vmulaz.xyzw $vf23, $vf24
# CHECK: vmulaw.xyzw     $$vf25, $$vf26          # encoding: [0xbf,0xc9,0xfa,0x4b]
vmulaw.xyzw $vf25, $vf26

# VMADDAbc - Vector Multiply-Add to ACC with broadcast
# CHECK: vmaddax.xyzw    $$vf1, $$vf2            # encoding: [0xbc,0x08,0xe2,0x4b]
vmaddax.xyzw $vf1, $vf2
# CHECK: vmadday.xyzw    $$vf3, $$vf4            # encoding: [0xbd,0x18,0xe4,0x4b]
vmadday.xyzw $vf3, $vf4
# CHECK: vmaddaz.xyzw    $$vf5, $$vf6            # encoding: [0xbe,0x28,0xe6,0x4b]
vmaddaz.xyzw $vf5, $vf6
# CHECK: vmaddaw.xyzw    $$vf7, $$vf8            # encoding: [0xbf,0x38,0xe8,0x4b]
vmaddaw.xyzw $vf7, $vf8

# VMSUBAbc - Vector Multiply-Subtract from ACC with broadcast
# CHECK: vmsubax.xyzw    $$vf9, $$vf10           # encoding: [0xfc,0x48,0xea,0x4b]
vmsubax.xyzw $vf9, $vf10
# CHECK: vmsubay.xyzw    $$vf11, $$vf12          # encoding: [0xfd,0x58,0xec,0x4b]
vmsubay.xyzw $vf11, $vf12

# VADDAbc - Vector Add to ACC with broadcast
# CHECK: vaddax.xyzw     $$vf13, $$vf14          # encoding: [0x3c,0x68,0xee,0x4b]
vaddax.xyzw $vf13, $vf14
# CHECK: vadday.xyzw     $$vf15, $$vf16          # encoding: [0x3d,0x78,0xf0,0x4b]
vadday.xyzw $vf15, $vf16

# VSUBAbc - Vector Subtract from ACC with broadcast
# CHECK: vsubax.xyzw     $$vf17, $$vf18          # encoding: [0x7c,0x88,0xf2,0x4b]
vsubax.xyzw $vf17, $vf18
# CHECK: vsubay.xyzw     $$vf19, $$vf20          # encoding: [0x7d,0x98,0xf4,0x4b]
vsubay.xyzw $vf19, $vf20
