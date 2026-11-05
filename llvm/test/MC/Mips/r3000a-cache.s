# RUN: llvm-mc -triple=mipsel-unknown-elf -mcpu=r3000a -show-encoding %s | FileCheck %s
# RUN: llvm-mc -triple=mipsel-unknown-elf -mcpu=r3000a -filetype=obj %s -o - | \
# RUN:   llvm-objdump -d --mcpu=r3000a - | FileCheck %s --check-prefix=DIS
# RUN: not llvm-mc -triple=mipsel-unknown-elf -mcpu=mips1 %s 2>&1 | \
# RUN:   FileCheck %s --check-prefix=MIPS1

# The R3000A accepts cache and pref for existing IOP assembly; plain MIPS I does not.

cache 1, 8($4)
# CHECK: cache 1, 8($4) # encoding: [0x08,0x00,0x81,0xbc]
# DIS: cache 0x1, 0x8($4)
# MIPS1: error: instruction requires a CPU feature not currently enabled
pref 0, 16($5)
# CHECK: pref 0, 16($5) # encoding: [0x10,0x00,0xa0,0xcc]
# DIS: pref 0x0, 0x10($5)
# MIPS1: error: instruction requires a CPU feature not currently enabled
