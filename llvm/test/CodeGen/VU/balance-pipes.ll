; RUN: llc -mtriple=vu -O3 -stop-after=vu-balance-pipes < %s | FileCheck %s

; In a loop with more upper than lower instructions, an insertion of field
; f + 1 into field f (x from y, y from z) is an MR32 in the lower pipe
; (INSERT_ROT) instead of an ADD in the upper one (INSERT_FIELD), until the
; pipes are balanced: here the first one moves, the second stays.

declare <4 x float> @llvm.vu.lq(i16)
declare void @llvm.vu.sq(<4 x float>, i16)
declare i16 @llvm.vu.ilw(i16, i32)

; CHECK-LABEL: name: busy_upper
; CHECK:       bb.1.loop:
; CHECK:         [[R0:%[0-9]+]]:vf = INSERT_ROT %{{[0-9]+}}, [[V:%[0-9]+]], 0
; CHECK-NEXT:    %{{[0-9]+}}:vf = INSERT_FIELD [[R0]], [[V]], 1, 2
define void @busy_upper() {
entry:
  %n = call i16 @llvm.vu.ilw(i16 0, i32 0)
  br label %loop
loop:
  %i = phi i16 [ 0, %entry ], [ %i.next, %loop ]
  %v = call <4 x float> @llvm.vu.lq(i16 %i)
  %m1 = fmul <4 x float> %v, %v
  %m2 = fmul <4 x float> %m1, %v
  %m3 = fadd <4 x float> %m2, %m1
  %m4 = fmul <4 x float> %m3, %m2
  %m5 = fadd <4 x float> %m4, %v
  %m6 = fmul <4 x float> %m5, %m5
  %y = extractelement <4 x float> %m6, i32 1
  %z = extractelement <4 x float> %m6, i32 2
  %r0 = insertelement <4 x float> %m5, float %y, i32 0
  %r1 = insertelement <4 x float> %r0, float %z, i32 1
  call void @llvm.vu.sq(<4 x float> %r1, i16 %i)
  %i.next = add i16 %i, 1
  %done = icmp eq i16 %i.next, %n
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
