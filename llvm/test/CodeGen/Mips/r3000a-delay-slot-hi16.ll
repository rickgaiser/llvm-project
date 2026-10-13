; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a < %s | FileCheck %s

; An instruction carrying a %hi relocation stays in front of its %lo in the
; same block: IRX1 relocations pair each HI16 with the LO16 that follows it, so
; the delay slot is not filled with the lui from the successor.

@g = global i32 7

define i32 @f(i32 %a, i32 %b) {
; CHECK-LABEL: f:
; CHECK:       bnez $4, $BB0_2
; CHECK-NEXT:  nop
; CHECK:       lui $1, %hi(g)
; CHECK-NEXT:  lw $1, %lo(g)($1)
entry:
  %c = icmp eq i32 %a, 0
  br i1 %c, label %t, label %e, !prof !0
t:
  %v = load i32, ptr @g
  %r = add i32 %v, %b
  ret i32 %r
e:
  %z = sub i32 %b, %a
  ret i32 %z
}

!0 = !{!"branch_weights", i32 99, i32 1}
