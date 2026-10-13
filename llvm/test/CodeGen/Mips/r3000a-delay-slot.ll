; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a < %s | FileCheck %s --check-prefix=FILL
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=mips1 -mattr=+delay-slot-succ-fill < %s | FileCheck %s --check-prefix=FILL
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=mips1 < %s | FileCheck %s --check-prefix=NOFILL
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a -mattr=-delay-slot-succ-fill < %s | FileCheck %s --check-prefix=NOFILL
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a -disable-mips-df-succbb-search=true < %s | FileCheck %s --check-prefix=NOFILL

; MIPS I has no branch-likely instructions. On the R3000A the delay slot of a
; conditional branch is filled from the likelier successor block when nothing
; before the branch can fill it, instead of with a nop.

define i32 @branch(i32 %a, i32 %b) {
; FILL-LABEL: branch:
; FILL:       beqz $4, $BB0_2
; FILL-NEXT:  subu $1, $5, $4
;
; NOFILL-LABEL: branch:
; NOFILL:       beqz $4, $BB0_2
; NOFILL-NEXT:  nop
entry:
  %c = icmp eq i32 %a, 0
  br i1 %c, label %t, label %e
t:
  %x = add i32 %b, 1
  %y = xor i32 %x, 9
  ret i32 %y
e:
  %z = sub i32 %b, %a
  %w = and i32 %z, 255
  ret i32 %w
}
