# RUN: llvm-mc -triple=dvpvu -show-encoding %s | FileCheck %s
# Test Integer ALU (Lower) instructions

# Integer Add
iadd        vi1, vi2, vi3
# CHECK: iadd vi1, vi2, vi3

# Integer Subtract
isub        vi4, vi5, vi6
# CHECK: isub vi4, vi5, vi6

# Integer And
iand        vi7, vi8, vi9
# CHECK: iand vi7, vi8, vi9

# Integer Or
ior         vi10, vi11, vi12
# CHECK: ior vi10, vi11, vi12

# Integer Add Immediate
iaddi       vi13, vi14, 5
# CHECK: iaddi vi13, vi14, 5

# Integer Subtract Immediate
isubi       vi15, vi1, 10
# CHECK: isubi vi15, vi1, 10
