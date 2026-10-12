# rfe exists on MIPS I and II only; later ISAs return from exceptions with eret.
# RUN: llvm-mc %s -triple=mips-unknown-linux -mcpu=mips1 -show-encoding | FileCheck %s --check-prefix=VALID
# RUN: llvm-mc %s -triple=mips-unknown-linux -mcpu=mips2 -show-encoding | FileCheck %s --check-prefix=VALID
# RUN: not llvm-mc %s -triple=mips-unknown-linux -mcpu=mips32 2>&1 | FileCheck %s --check-prefix=INVALID
# RUN: not llvm-mc %s -triple=mips64-unknown-linux -mcpu=mips3 2>&1 | FileCheck %s --check-prefix=INVALID
# RUN: not llvm-mc %s -triple=mips64-unknown-linux -mcpu=mips64 2>&1 | FileCheck %s --check-prefix=INVALID
# RUN: not llvm-mc %s -triple=mips64el-unknown-linux -mcpu=r5900 2>&1 | FileCheck %s --check-prefix=INVALID

        rfe  # VALID: rfe # encoding: [0x42,0x00,0x00,0x10]
             # INVALID: error: instruction requires a CPU feature not currently enabled
