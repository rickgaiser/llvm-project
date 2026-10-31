# RUN: llvm-mc %s -triple=mips64el-unknown-linux -mcpu=r5900 -show-encoding \
# RUN:   | FileCheck %s

# The R5900 supports the MIPS IV integer conditional moves (MOVN, MOVZ) and
# PREF. The FPU conditional moves and MOVT/MOVF are not supported, see
# r5900-invalid.s.

# CHECK: movn    $12, $13, $14           # encoding: [0x0b,0x60,0xae,0x01]
movn    $t0, $t1, $t2

# CHECK: movn    $2, $3, $4              # encoding: [0x0b,0x10,0x64,0x00]
movn    $v0, $v1, $a0

# CHECK: movz    $12, $13, $14           # encoding: [0x0a,0x60,0xae,0x01]
movz    $t0, $t1, $t2

# CHECK: movz    $2, $3, $4              # encoding: [0x0a,0x10,0x64,0x00]
movz    $v0, $v1, $a0

# CHECK: pref    0, 16($4)               # encoding: [0x10,0x00,0x80,0xcc]
pref    0, 16($a0)

# CHECK: pref    4, -8($sp)              # encoding: [0xf8,0xff,0xa4,0xcf]
pref    4, -8($sp)
