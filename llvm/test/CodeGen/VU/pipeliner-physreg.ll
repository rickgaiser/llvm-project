; RUN: llc -mtriple=vu -verify-machineinstrs -debug-only=pipeliner < %s \
; RUN:   -o /dev/null 2>&1 | FileCheck %s
; RUN: llc -mtriple=vu -pass-remarks-analysis=pipeliner < %s -o /dev/null 2>&1 \
; RUN:   | FileCheck %s --check-prefix=MII
; REQUIRES: asserts

; MII: Minimal Initiation Interval: 7 (ResMII 7, RecMII 2)

; A square root and two divisions per iteration, all through Q, which is not
; renamed between iterations. A schedule in which an iteration writes Q while
; the value of another iteration is still needed is rejected.

; CHECK: Invalid schedule: $q is live from cycle

declare <4 x float> @llvm.vu.lq(i16)
declare void @llvm.vu.sq(<4 x float>, i16)
declare float @llvm.maxnum.f32(float, float)
declare float @llvm.sqrt.f32(float)

define void @three_divisions() {
entry:
  br label %loop

loop:
  %i = phi i16 [ 10, %entry ], [ %i.next, %loop ]
  %v = call <4 x float> @llvm.vu.lq(i16 %i)
  %x = extractelement <4 x float> %v, i32 0
  %y = extractelement <4 x float> %v, i32 1
  %z = extractelement <4 x float> %v, i32 2
  %a = call float @llvm.sqrt.f32(float %x)
  %b = fdiv float %y, %a
  %c = fdiv float 1.0, %z
  %m = fmul float %b, %c
  %res = call float @llvm.maxnum.f32(float %m, float %y)
  %r = insertelement <4 x float> %v, float %res, i32 3
  %o = add i16 %i, 100
  call void @llvm.vu.sq(<4 x float> %r, i16 %o)
  %i.next = add i16 %i, 1
  %done = icmp eq i16 %i.next, 50
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
