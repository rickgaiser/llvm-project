; RUN: llc -mtriple=vu -pass-remarks-analysis=pipeliner < %s -o /dev/null 2>&1 \
; RUN:   | FileCheck %s
; RUN: llc -mtriple=vu -debug-only=vu-instr-info < %s -o /dev/null 2>&1 \
; RUN:   | FileCheck %s --check-prefix=COPIES
; REQUIRES: asserts

; Loops are not unrolled, so a value that lives longer than one iteration is
; copied in every iteration, with a MOVE in the lower slot. A schedule is only
; used if its instructions, the copies and the branch fit in II words: the
; load, the store, the increment and the branch do not fit in 3 words, and at
; II 4 the product lives longer than an iteration.
; CHECK: Minimal Initiation Interval: 3 (ResMII 3, RecMII 2)
; CHECK: Schedule found with Initiation Interval: 5
; COPIES: VU: II 3 needs 1 upper and 6 lower words with copies
; COPIES: VU: II 4 needs 1 upper and 5 lower words with copies
; COPIES: VU: II 5 needs 1 upper and 4 lower words with copies

declare <4 x float> @llvm.vu.lq(i16)
declare void @llvm.vu.sq(<4 x float>, i16)

define void @scale_loop() {
entry:
  br label %loop

loop:
  %i = phi i16 [ 0, %entry ], [ %i.next, %loop ]
  %v = call <4 x float> @llvm.vu.lq(i16 %i)
  %s = fmul <4 x float> %v, <float 3.0, float 3.0, float 3.0, float 3.0>
  call void @llvm.vu.sq(<4 x float> %s, i16 %i)
  %i.next = add i16 %i, 1
  %done = icmp eq i16 %i.next, 100
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
