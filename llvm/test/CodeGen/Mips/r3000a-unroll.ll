; RUN: opt -mtriple=mipsel-none-elf -mcpu=r3000a -passes='loop-unroll<O2>' -S < %s | FileCheck %s

; A short loop is unrolled at -O2, at most 4 times, and never at -Os: the IOP has
; little memory.

; CHECK-LABEL: @sum(
; CHECK: unroll_iter
; CHECK-COUNT-4: load i32
; CHECK-NOT: load i32
; CHECK: for.end
define i32 @sum(ptr %p, i32 %n) {
entry:
  br label %for.body
for.body:
  %i = phi i32 [ 0, %entry ], [ %i1, %for.body ]
  %s = phi i32 [ 0, %entry ], [ %s1, %for.body ]
  %q = getelementptr i32, ptr %p, i32 %i
  %v = load i32, ptr %q
  %s1 = add i32 %s, %v
  %i1 = add i32 %i, 1
  %c = icmp ne i32 %i1, %n
  br i1 %c, label %for.body, label %for.end
for.end:
  ret i32 %s1
}

; CHECK-LABEL: @sum_os(
; CHECK-COUNT-1: load i32
; CHECK-NOT: load i32
define i32 @sum_os(ptr %p, i32 %n) optsize {
entry:
  br label %for.body
for.body:
  %i = phi i32 [ 0, %entry ], [ %i1, %for.body ]
  %s = phi i32 [ 0, %entry ], [ %s1, %for.body ]
  %q = getelementptr i32, ptr %p, i32 %i
  %v = load i32, ptr %q
  %s1 = add i32 %s, %v
  %i1 = add i32 %i, 1
  %c = icmp ne i32 %i1, %n
  br i1 %c, label %for.body, label %for.end
for.end:
  ret i32 %s1
}
