# RUN: llvm-mc %s -triple=mips64el-unknown-linux -mcpu=r5900 -show-encoding \
# RUN:   | FileCheck %s

# The R5900 supports a subset of MIPS IV: MOVN, MOVZ and PREF. The
# FPU conditional moves and MOVT/MOVF are not supported, see r5900-invalid.s.

# CHECK: movn    $12, $13, $14           # encoding: [0x0b,0x60,0xae,0x01]
movn    $t0, $t1, $t2

# CHECK: movn    $2, $3, $4              # encoding: [0x0b,0x10,0x64,0x00]
movn    $v0, $v1, $a0

# CHECK: movz    $12, $13, $14           # encoding: [0x0a,0x60,0xae,0x01]
movz    $t0, $t1, $t2

# CHECK: movz    $2, $3, $4              # encoding: [0x0a,0x10,0x64,0x00]
movz    $v0, $v1, $a0

# The R5900 only defines hint 0 (load); the other hints are reserved.
# CHECK: pref    0, 0($12)               # encoding: [0x00,0x00,0x80,0xcd]
pref    0, 0($t0)

# CHECK: pref    0, -16($sp)             # encoding: [0xf0,0xff,0xa0,0xcf]
pref    0, -16($sp)
