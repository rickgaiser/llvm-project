# RUN: llvm-mc %s -triple=mips64el-none-elf -mcpu=r5900 -show-encoding \
# RUN:   | FileCheck %s

# Test R5900 MIPS IV instructions
# The R5900 supports a subset of MIPS IV: MOVN, MOVZ, PREF, and related FPU moves

# Integer conditional moves
# CHECK: movn    $12, $13, $14           # encoding: [0x0b,0x60,0xae,0x01]
movn    $t0, $t1, $t2

# CHECK: movn    $2, $3, $4              # encoding: [0x0b,0x10,0x64,0x00]
movn    $v0, $v1, $a0

# CHECK: movz    $12, $13, $14           # encoding: [0x0a,0x60,0xae,0x01]
movz    $t0, $t1, $t2

# CHECK: movz    $2, $3, $4              # encoding: [0x0a,0x10,0x64,0x00]
movz    $v0, $v1, $a0

# FPU conditional moves (single-precision only on R5900)
# CHECK: movn.s  $f0, $f1, $12           # encoding: [0x13,0x08,0x0c,0x46]
movn.s  $f0, $f1, $t0

# CHECK: movn.s  $f2, $f3, $13           # encoding: [0x93,0x18,0x0d,0x46]
movn.s  $f2, $f3, $t1

# CHECK: movz.s  $f4, $f5, $14           # encoding: [0x12,0x29,0x0e,0x46]
movz.s  $f4, $f5, $t2

# CHECK: movz.s  $f6, $f7, $4            # encoding: [0x92,0x39,0x04,0x46]
movz.s  $f6, $f7, $a0

# Prefetch instruction
# CHECK: pref    0, 0($12)               # encoding: [0x00,0x00,0x80,0xcd]
pref    0, 0($t0)

# CHECK: pref    1, 16($13)              # encoding: [0x10,0x00,0xa1,0xcd]
pref    1, 16($t1)

# CHECK: pref    4, -4($14)              # encoding: [0xfc,0xff,0xc4,0xcd]
pref    4, -4($t2)

# CHECK: pref    5, 128($sp)             # encoding: [0x80,0x00,0xa5,0xcf]
pref    5, 128($sp)

# CHECK: pref    6, 0($zero)             # encoding: [0x00,0x00,0x06,0xcc]
pref    6, 0($zero)

# Prefetch hint values commonly used:
# 0 = load
# 1 = store
# 4 = load_streamed
# 5 = store_streamed
# 6 = load_retained
# 7 = store_retained
# CHECK: pref    7, 64($4)               # encoding: [0x40,0x00,0x87,0xcc]
pref    7, 64($a0)
