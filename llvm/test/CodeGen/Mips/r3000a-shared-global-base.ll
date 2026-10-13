; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a -mips-split-hi16 < %s | FileCheck %s --check-prefix=SHARED
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=mips1 -mattr=+shared-global-base -mips-split-hi16 < %s | FileCheck %s --check-prefix=SHARED
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=mips1 -mips-split-hi16 < %s | FileCheck %s --check-prefix=SPLIT
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a -mattr=-shared-global-base -mips-split-hi16 < %s | FileCheck %s --check-prefix=SPLIT

; IRX1 pairs every %hi with the %lo that follows it, so with -mips-split-hi16
; each access to a global gets its own lui. When a function reaches one global
; three or more times, the R3000A builds the address once (an adjacent lui and
; addiu pair) and reaches the members through register + offset.

%S = type { i32, i32, i32, i32, [8 x i32] }
@g = global %S zeroinitializer
@h = global i32 0

declare void @use(i32)

define void @many(i32 %x) {
; SHARED-LABEL: many:
; SHARED:       lui $[[HI:[0-9]+]], %hi(g)
; SHARED-NEXT:  addiu $[[BASE:[0-9]+]], $[[HI]], %lo(g)
; SHARED:       sw $4, 0($[[BASE]])
; SHARED:       lw $4, 8($[[BASE]])
; SHARED-NOT:   %lo(g)($
;
; SPLIT-LABEL: many:
; SPLIT:       lui $1, %hi(g)
; SPLIT-NEXT:  sw $4, %lo(g)($1)
; SPLIT:       lui {{.*}}, %hi(g)
  store i32 %x, ptr @g
  %p1 = getelementptr %S, ptr @g, i32 0, i32 1
  store i32 1, ptr %p1
  %p2 = getelementptr %S, ptr @g, i32 0, i32 2
  %v = load i32, ptr %p2
  %p3 = getelementptr %S, ptr @g, i32 0, i32 4, i32 3
  store i32 %v, ptr %p3
  call void @use(i32 %v)
  ret void
}

define i32 @two(i32 %x) {
; Two accesses cost the same either way, so each keeps its own lui.
; SHARED-LABEL: two:
; SHARED:       lui {{.*}}, %hi(h)
; SHARED-NEXT:  lw {{.*}}, %lo(h)
; SPLIT-LABEL: two:
; SPLIT:        lui {{.*}}, %hi(h)
  %a = load i32, ptr @h
  %b = add i32 %a, %x
  store i32 %b, ptr @h
  ret i32 %b
}
