; RUN: opt -disable-output < %s -aa-pipeline=basic-aa,scev-aa -passes=aa-eval \
; RUN:   -print-all-alias-modref-info 2>&1 | FileCheck %s

; BasicAA queries SCEVAA for the incoming pointers of a phi with an unknown
; size. With 32-bit pointers, that size must be the largest 32-bit one.

target datalayout = "e-p:32:32"

; CHECK-LABEL: Function: phi
; CHECK: MayAlias:	i32* %q, i32* %r
define void @phi(ptr %a, i32 %n) {
entry:
  br label %loop

loop:
  %p = phi ptr [ %a, %entry ], [ %p.next, %loop ]
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %q = getelementptr i8, ptr %p, i32 8
  %r = getelementptr i8, ptr %a, i32 4
  store i32 0, ptr %q
  %v = load i32, ptr %r
  %p.next = getelementptr i8, ptr %p, i32 16
  %i.next = add i32 %i, 1
  %c = icmp ne i32 %i.next, %n
  br i1 %c, label %loop, label %exit

exit:
  ret void
}
