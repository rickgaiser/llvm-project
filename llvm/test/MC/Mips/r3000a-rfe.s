# rfe is accepted only for the PS2 IOP (R3000A).
# RUN: llvm-mc %s -triple=mipsel-unknown-linux -mcpu=r3000a -show-encoding | FileCheck %s --check-prefix=VALID
# RUN: not llvm-mc %s -triple=mips-unknown-linux -mcpu=mips1 2>&1 | FileCheck %s --check-prefix=INVALID
# RUN: not llvm-mc %s -triple=mips-unknown-linux -mcpu=mips2 2>&1 | FileCheck %s --check-prefix=INVALID
# RUN: not llvm-mc %s -triple=mips-unknown-linux -mcpu=mips32 2>&1 | FileCheck %s --check-prefix=INVALID
# RUN: not llvm-mc %s -triple=mips64el-unknown-linux -mcpu=r5900 2>&1 | FileCheck %s --check-prefix=INVALID

        rfe  # VALID: rfe # encoding: [0x10,0x00,0x00,0x42]
             # INVALID: error: instruction requires a CPU feature not currently enabled
