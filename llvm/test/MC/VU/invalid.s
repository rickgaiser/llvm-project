; RUN: not llvm-mc -triple=vu %s 2>&1 | FileCheck %s

.vu
  nop    lq.xyzw vf1, 1024(vi2)
; CHECK: :[[@LINE-1]]:23: error: immediate must be an integer in the range [-1024, 1023]
  nop    iaddiu vi1, vi2, 0x8000
; CHECK: :[[@LINE-1]]:27: error: immediate must be an integer in the range [0, 32767]
  nop    iaddi vi1, vi2, 16
; CHECK: :[[@LINE-1]]:26: error: immediate must be an integer in the range [-16, 15]
  nop    fsset 0x1000
; CHECK: :[[@LINE-1]]:16: error: immediate must be an integer in the range [0, 4095]
  nop    fcset 0x1000000
; CHECK: :[[@LINE-1]]:16: error: immediate must be an integer in the range [0, 16777215]
  nop    b 1024
; CHECK: :[[@LINE-1]]:12: error: branch target must be a label or an integer in the range [-1024, 1023]
  nop    ilw.xy vi1, 0(vi2)
; CHECK: :[[@LINE-1]]:10: error: destination must be a single field (x, y, z or w)
  add.yx vf1, vf2, vf3    nop
; CHECK: :[[@LINE-1]]:3: error: invalid field mask '.yx'
  add.xyzw vf1, vf2, vf32    nop
; CHECK: :[[@LINE-1]]:22: error: invalid operand for instruction
  nop    div q, vf1, vf2y
; CHECK: :[[@LINE-1]]:22: error: expected a register with a field suffix (x, y, z or w)
  nop    iadd.x vi1, vi2, vi3
; CHECK: :[[@LINE-1]]:10: error: unrecognized instruction mnemonic, did you mean: addx, iadd?
  clipw.xy vf1, vf2w    nop
; CHECK: :[[@LINE-1]]:3: error: 'clipw' only supports the .xyz field mask
  lq.xyzw vf1, 0(vi2)    nop
; CHECK: :[[@LINE-1]]:3: error: expected an upper instruction
  nop    add.xyzw vf1, vf2, vf3
; CHECK: :[[@LINE-1]]:10: error: expected a lower instruction
  nop    b[e] 0
; CHECK: :[[@LINE-1]]:11: error: flags are only allowed on the first instruction
  nop[x]    nop
; CHECK: :[[@LINE-1]]:7: error: unknown flag 'x'
  nop    fcand vi2, 1
; CHECK: :[[@LINE-1]]:16: error: invalid operand for instruction
  adda.xyzw vf1, vf2, vf3    nop
; CHECK: :[[@LINE-1]]:13: error: invalid operand for instruction
  nop    lqi.xyzw vf1, (vi2+)
; CHECK: :[[@LINE-1]]:29: error: expected '++'
  frob.xyzw vf1, vf2    nop
; CHECK: :[[@LINE-1]]:3: error: unrecognized instruction mnemonic
