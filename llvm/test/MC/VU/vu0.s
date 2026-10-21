; RUN: not llvm-mc -triple=vu -mcpu=vu0 %s 2>&1 | FileCheck %s --check-prefix=VU0
; RUN: llvm-mc -triple=vu -mcpu=vu1 -show-encoding %s 2>&1 | FileCheck %s --check-prefix=VU1

; The EFU, XGKICK and XTOP only exist on VU1.

.vu
  nop    esin p, vf2w
; VU0: :[[@LINE-1]]:10: error: instruction requires: vu1
; VU1: nop esin p, vf2w ; encoding: [0xfc,0x17,0x60,0x80,0xff,0x02,0x00,0x00]
  nop    mfp.x vf1, p
; VU0: :[[@LINE-1]]:10: error: instruction requires: vu1
  nop    waitp
; VU0: :[[@LINE-1]]:10: error: instruction requires: vu1
  nop    xgkick vi2
; VU0: :[[@LINE-1]]:10: error: instruction requires: vu1
  nop    xtop vi1
; VU0: :[[@LINE-1]]:10: error: instruction requires: vu1
