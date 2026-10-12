; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=mips1 -relocation-model=static \
; RUN:     -mattr=+soft-float -mips-split-hi16 -filetype=obj < %s \
; RUN:   | llvm-readobj --relocations - | FileCheck %s

; Each R_MIPS_HI16 is followed directly by the R_MIPS_LO16 it belongs to,
; as the PS2 IRX1 module format requires.

@g = global [4 x i32] zeroinitializer, align 4

define i32 @f() {
entry:
  %p1 = getelementptr [4 x i32], ptr @g, i32 0, i32 1
  %p3 = getelementptr [4 x i32], ptr @g, i32 0, i32 3
  %a = load volatile i32, ptr %p1
  store volatile i32 %a, ptr @g
  %b = load volatile i32, ptr %p3
  ret i32 %b
}

; CHECK:      R_MIPS_HI16 g
; CHECK-NEXT: R_MIPS_LO16 g
; CHECK-NEXT: R_MIPS_HI16 g
; CHECK-NEXT: R_MIPS_LO16 g
; CHECK-NEXT: R_MIPS_HI16 g
; CHECK-NEXT: R_MIPS_LO16 g
