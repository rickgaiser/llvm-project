; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a < %s | FileCheck %s --check-prefix=R3000A
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=mips1 < %s | FileCheck %s --check-prefix=MIPS1

; The R3000A scheduling model knows a load result is not available to the next
; instruction, so independent loads are issued back to back and no nop is
; needed after them. Plain MIPS I has no model and keeps the load delay nops.

define i32 @load_pair(ptr %p, ptr %q, i32 %i) {
; R3000A-LABEL: load_pair:
; R3000A:       lw
; R3000A-NEXT:  lw
; R3000A-NOT:   nop
; R3000A:       jr $ra
;
; MIPS1-LABEL: load_pair:
; MIPS1:       lw
; MIPS1-NEXT:  nop
  %a = load i32, ptr %p
  %b = load i32, ptr %q
  %c = add i32 %a, %i
  %d = add i32 %b, %i
  %r = xor i32 %c, %d
  ret i32 %r
}
