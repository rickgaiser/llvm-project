# RUN: llvm-mc -triple=dvpvu -show-encoding %s | FileCheck %s
# Test Branch (Lower) instructions

# Unconditional Branch
b           100
# CHECK: b 100

# Branch and Link
bal         vi11, 200
# CHECK: bal vi11, 200

# Jump Register
jr          vi1
# CHECK: jr vi1

# Jump and Link Register
jalr        vi11, vi2
# CHECK: jalr vi11, vi2

# Branch if Equal
ibeq        vi3, vi4, 50
# CHECK: ibeq vi3, vi4, 50

# Branch if Not Equal
ibne        vi5, vi6, 60
# CHECK: ibne vi5, vi6, 60

# Branch if Greater Than Zero
ibgtz       vi7, 70
# CHECK: ibgtz vi7, 70

# Branch if Greater or Equal to Zero
ibgez       vi8, 80
# CHECK: ibgez vi8, 80

# Branch if Less Than Zero
ibltz       vi9, 90
# CHECK: ibltz vi9, 90

# Branch if Less or Equal to Zero
iblez       vi10, 100
# CHECK: iblez vi10, 100
