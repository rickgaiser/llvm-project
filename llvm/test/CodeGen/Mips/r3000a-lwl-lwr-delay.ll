; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a -O2 < %s | FileCheck %s
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=mips1 -O2 < %s | FileCheck %s --check-prefix=MIPS1

; The R3000A merges an LWL/LWR pair into one register through the load delay,
; so the second load needs no nop in front of it. Other MIPS I CPUs keep the nop.

define i32 @load_unaligned(ptr %p) {
; CHECK-LABEL: load_unaligned:
; CHECK:         lwl $2, 3($4)
; CHECK-NEXT:    lwr $2, 0($4)
; CHECK-NEXT:    jr $ra
; CHECK-NEXT:    nop
;
; MIPS1-LABEL: load_unaligned:
; MIPS1:         lwl $2, 3($4)
; MIPS1-NEXT:    nop
; MIPS1-NEXT:    lwr $2, 0($4)
  %v = load i32, ptr %p, align 1
  ret i32 %v
}

define i32 @load_unaligned_add(ptr %p, i32 %k) {
; CHECK-LABEL: load_unaligned_add:
; CHECK:         lwl $1, 3($4)
; CHECK-NEXT:    lwr $1, 0($4)
; CHECK-NEXT:    jr $ra
; CHECK-NEXT:    addu $2, $1, $5
  %v = load i32, ptr %p, align 1
  %r = add i32 %v, %k
  ret i32 %r
}
