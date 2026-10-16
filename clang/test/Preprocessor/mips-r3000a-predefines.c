// RUN: %clang_cc1 -E -dM -triple mipsel-unknown-elf -target-cpu r3000a \
// RUN:   -target-abi o32 < /dev/null | FileCheck %s
// RUN: %clang_cc1 -E -dM -triple mipsel-unknown-elf -target-cpu mips32 \
// RUN:   -target-abi o32 < /dev/null | FileCheck --check-prefix=MIPS32 %s

// CHECK: #define _MIPS_ISA _MIPS_ISA_MIPS1
// CHECK: #define __mips 1
// MIPS32: #define _MIPS_ISA _MIPS_ISA_MIPS32
// MIPS32: #define __mips 32
