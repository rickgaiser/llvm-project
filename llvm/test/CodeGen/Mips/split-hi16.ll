; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=mips1 -relocation-model=static \
; RUN:     -mattr=+soft-float < %s | FileCheck %s --check-prefix=SHARED
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=mips1 -relocation-model=static \
; RUN:     -mattr=+soft-float -mips-split-hi16 < %s | FileCheck %s --check-prefix=SPLIT
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a -relocation-model=static \
; RUN:     -mattr=+soft-float -mips-split-hi16=false < %s | FileCheck %s --check-prefix=SHARED

; Without the option one lui feeds every access to @g. With it, each access
; has its own lui straight in front of it, so every R_MIPS_HI16 has exactly
; one R_MIPS_LO16 right behind it.

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

; SHARED:      lui $[[R:[0-9]+]], %hi(g)
; SHARED-NEXT: lw ${{[0-9]+}}, %lo(g+4)($[[R]])
; SHARED-NOT:  lui
; SHARED:      sw ${{[0-9]+}}, %lo(g)($[[R]])
; SHARED-NOT:  lui
; SHARED:      lw ${{[0-9]+}}, %lo(g+12)($[[R]])

; SPLIT:      lui $[[R1:[0-9]+]], %hi(g)
; SPLIT-NEXT: lw ${{[0-9]+}}, %lo(g+4)($[[R1]])
; SPLIT:      lui $[[R2:[0-9]+]], %hi(g)
; SPLIT-NEXT: sw ${{[0-9]+}}, %lo(g)($[[R2]])
; SPLIT:      lui $[[R3:[0-9]+]], %hi(g)
; SPLIT-NEXT: lw ${{[0-9]+}}, %lo(g+12)($[[R3]])
