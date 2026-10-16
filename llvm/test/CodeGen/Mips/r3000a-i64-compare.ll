; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a -O2 < %s | FileCheck %s --check-prefix=R3000A
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=mips1 -O2 < %s | FileCheck %s --check-prefix=MIPS1

; R3000A i64 orderings compare the high words with the borrow of the low words
; and need no branch.

define i32 @ult(i64 %a, i64 %b) {
; R3000A-LABEL: ult:
; R3000A-NOT:     b{{n?}}ez
; R3000A:         sltu $1, $4, $6
; R3000A-NEXT:    subu $2, $5, $7
; R3000A-NEXT:    sltu $1, $2, $1
; R3000A-NEXT:    sltu $2, $5, $7
; R3000A-NEXT:    jr $ra
; R3000A-NEXT:    or $2, $2, $1
;
; MIPS1-LABEL: ult:
; MIPS1:         b{{n?}}ez
  %c = icmp ult i64 %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}

define i32 @sge(i64 %a, i64 %b) {
; R3000A-LABEL: sge:
; R3000A-NOT:     b{{n?}}ez
; R3000A:         slt
; R3000A:         jr $ra
; R3000A-NEXT:    xori $2, $1, 1
  %c = icmp sge i64 %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}
