; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a -mattr=+noabicalls < %s | FileCheck %s --check-prefix=TAIL
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=mips1 -mattr=+noabicalls -mips-tail-calls < %s | FileCheck %s --check-prefix=TAIL
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=mips1 -mattr=+noabicalls < %s | FileCheck %s --check-prefix=CALL
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a -mattr=+noabicalls -mips-tail-calls=false < %s | FileCheck %s --check-prefix=CALL
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a < %s | FileCheck %s --check-prefix=CALL

; A call in tail position becomes a jump when the callee's arguments fit in the
; caller's incoming argument area, so the caller needs no frame and no
; callee-saved register to hold its arguments across the call. With SVR4
; abicalls (the default) a call to another module needs $gp restored, so it
; stays a call.

declare i32 @callee(i32, i32, i32, i32, i32)
declare i32 @few(i32)

define i32 @forward(i32 %a, i32 %b, i32 %c, i32 %d, i32 %e) {
; TAIL-LABEL: forward:
; TAIL-NOT:   addiu $sp
; TAIL:       j callee
;
; CALL-LABEL: forward:
; CALL:       jal callee
  %r = tail call i32 @callee(i32 %a, i32 %b, i32 %c, i32 %d, i32 %e)
  ret i32 %r
}

define i32 @after_work(i32 %a) {
; Work before the call stays; only the call itself becomes a jump.
; TAIL-LABEL: after_work:
; TAIL:       j few
; TAIL-NEXT:  addiu $4, $4, 1
;
; CALL-LABEL: after_work:
; CALL:       jal few
  %x = add i32 %a, 1
  %r = tail call i32 @few(i32 %x)
  ret i32 %r
}
