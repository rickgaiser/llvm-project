; RUN: llc -mtriple=mipsel-unknown-elf -mattr=+r3000a < %s 2>&1 | FileCheck %s
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a -mattr=+r3000a < %s 2>&1 | FileCheck %s

; CHECK-NOT: not a recognized feature
; CHECK: jr $ra

define i32 @f(i32 %a) {
  %r = mul i32 %a, 3
  ret i32 %r
}
