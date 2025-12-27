# RUN: llvm-mc %s -triple=mips64el-unknown-linux -mcpu=r5900 -show-encoding \
# RUN:   | FileCheck %s

# Test R5900 EE Core dual pipeline integer instructions

# Pipeline 0: 3-operand multiply (SPECIAL opcode)
# CHECK: mult $4, $2, $3      # encoding: [0x18,0x20,0x43,0x00]
mult $4, $2, $3

# CHECK: multu $5, $6, $7     # encoding: [0x19,0x28,0xc7,0x00]
multu $5, $6, $7

# Pipeline 0: MADD/MADDU (MMI opcode)
# CHECK: madd $4, $2, $3      # encoding: [0x00,0x20,0x43,0x70]
madd $4, $2, $3

# CHECK: maddu $5, $6, $7     # encoding: [0x01,0x28,0xc7,0x70]
maddu $5, $6, $7

# Pipeline 1: Move from/to HI1/LO1
# CHECK: mfhi1 $10            # encoding: [0x10,0x50,0x00,0x70]
mfhi1 $10

# CHECK: mflo1 $11            # encoding: [0x12,0x58,0x00,0x70]
mflo1 $11

# CHECK: mthi1 $12            # encoding: [0x11,0x00,0x80,0x71]
mthi1 $12

# CHECK: mtlo1 $13            # encoding: [0x13,0x00,0xa0,0x71]
mtlo1 $13

# Pipeline 1: Multiply
# CHECK: mult1 $4, $2, $3     # encoding: [0x18,0x20,0x43,0x70]
mult1 $4, $2, $3

# CHECK: multu1 $5, $6, $7    # encoding: [0x19,0x28,0xc7,0x70]
multu1 $5, $6, $7

# Pipeline 1: Divide
# CHECK: div1 $zero, $6, $7   # encoding: [0x1a,0x00,0xc7,0x70]
div1 $zero, $6, $7

# CHECK: divu1 $zero, $8, $9  # encoding: [0x1b,0x00,0x09,0x71]
divu1 $zero, $8, $9

# Pipeline 1: Multiply-Add
# CHECK: madd1 $4, $2, $3     # encoding: [0x20,0x20,0x43,0x70]
madd1 $4, $2, $3

# CHECK: maddu1 $5, $6, $7    # encoding: [0x21,0x28,0xc7,0x70]
maddu1 $5, $6, $7
