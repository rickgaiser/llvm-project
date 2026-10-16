; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a -O2 < %s | FileCheck %s --check-prefix=R3000A
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a -O2 -post-RA-scheduler=false < %s \
; RUN:   | FileCheck %s --check-prefix=NOSCHED

; The post-RA scheduler runs for the R3000A at -O2, so the epilogue reloads
; move up past the body's arithmetic. Other MIPS I CPUs run it only at -O3.

declare void @g(i32)

define i32 @f(ptr %a, ptr %b) {
; R3000A-LABEL: f:
; R3000A:         lw $3, 8($16)
; R3000A-NEXT:    addu $1, $1, $2
; R3000A-NEXT:    lw $ra, 28($sp)
; R3000A-NEXT:    lw $16, 20($sp)
; R3000A-NEXT:    lw $17, 24($sp)
; R3000A-NEXT:    xor $2, $1, $3
; R3000A-NEXT:    jr $ra
;
; NOSCHED-LABEL: f:
; NOSCHED:         lw $3, 8($16)
; NOSCHED-NEXT:    addu $1, $1, $2
; NOSCHED-NEXT:    xor $2, $1, $3
; NOSCHED-NEXT:    lw $ra, 28($sp)
  %x = load i32, ptr %a
  call void @g(i32 %x)
  %y = load i32, ptr %b
  %p = getelementptr i32, ptr %b, i32 1
  %z = load i32, ptr %p
  %s = add i32 %y, %z
  %q = getelementptr i32, ptr %a, i32 2
  %w = load i32, ptr %q
  %t = xor i32 %s, %w
  ret i32 %t
}
