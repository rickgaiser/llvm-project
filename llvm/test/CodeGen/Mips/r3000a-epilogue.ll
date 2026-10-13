; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a < %s | FileCheck %s --check-prefix=FIRST
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=mips1 -mattr=+ra-restore-first < %s | FileCheck %s --check-prefix=FIRST
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=mips1 < %s | FileCheck %s --check-prefix=LAST
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a -mattr=-ra-restore-first < %s | FileCheck %s --check-prefix=LAST

; MIPS I has no load interlock, so a load directly before its use needs a nop.
; The return reads $ra: restoring it last costs a nop before the jump, while
; restoring it first lets the other restore fill the delay.

declare i32 @g(i32)

define i32 @f(i32 %x) {
; FIRST-LABEL: f:
; FIRST:       lw $ra, 20($sp)
; FIRST-NEXT:  lw $16, 16($sp)
; FIRST-NEXT:  jr $ra
; FIRST-NEXT:  addiu $sp, $sp, 24
;
; LAST-LABEL: f:
; LAST:       lw $16, 16($sp)
; LAST-NEXT:  lw $ra, 20($sp)
; LAST-NEXT:  nop
; LAST-NEXT:  jr $ra
  %a = call i32 @g(i32 %x)
  %b = call i32 @g(i32 %a)
  %c = add i32 %a, %b
  ret i32 %c
}
