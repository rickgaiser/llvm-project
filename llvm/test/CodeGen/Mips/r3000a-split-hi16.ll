; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a -relocation-model=static \
; RUN:     -mattr=+soft-float < %s | FileCheck %s
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a -relocation-model=static \
; RUN:     -mattr=+soft-float -filetype=obj < %s \
; RUN:   | llvm-readobj --relocations - | FileCheck %s --check-prefix=RELOC
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a -relocation-model=static \
; RUN:     -mattr=+soft-float -mips-split-hi16=false < %s \
; RUN:   | FileCheck %s --check-prefix=OFF

; The R3000A splits %hi uses without -mips-split-hi16: its objects become IOP
; modules, where each R_MIPS_HI16 pairs with the R_MIPS_LO16 right behind it.

@g = global [4 x i32] zeroinitializer, align 4

define i32 @f() {
entry:
  %p3 = getelementptr [4 x i32], ptr @g, i32 0, i32 3
  %a = load volatile i32, ptr @g
  %b = load volatile i32, ptr %p3
  %r = add i32 %a, %b
  ret i32 %r
}

; CHECK:      lui $[[R1:[0-9]+]], %hi(g)
; CHECK-NEXT: lw ${{[0-9]+}}, %lo(g)($[[R1]])
; CHECK:      lui $[[R2:[0-9]+]], %hi(g)
; CHECK-NEXT: lw ${{[0-9]+}}, %lo(g+12)($[[R2]])

; RELOC:      R_MIPS_HI16 g
; RELOC-NEXT: R_MIPS_LO16 g
; RELOC-NEXT: R_MIPS_HI16 g
; RELOC-NEXT: R_MIPS_LO16 g

; OFF-COUNT-1: lui
; OFF-NOT:     lui
