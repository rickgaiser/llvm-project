; RUN: llc -mtriple=vu -O3 -stop-after=vu-loop-alias-scopes < %s | FileCheck %s

; The loads and stores of a loop through the same base whose bytes over all
; iterations are disjoint get alias scopes, noalias to each other. The bound
; uses c * (n /u c) <= n: the outputs follow the inputs as in a compute
; program, x[n + 1 + 3 g] = f(x[4 g]) for g < n / 4.

declare i16 @llvm.vu.ilw(i16, i32)

; CHECK-LABEL: define void @after_inputs(
; CHECK: load <4 x float>, ptr %pi, align 16, !alias.scope [[IN:![0-9]+]], !noalias [[OUT:![0-9]+]]
; CHECK: store <4 x float> %y, ptr %po, align 16, !alias.scope [[OUT]], !noalias [[IN]]
define void @after_inputs() {
entry:
  %n = call i16 @llvm.vu.ilw(i16 0, i32 0)
  %m = lshr i16 %n, 2
  %empty = icmp eq i16 %m, 0
  br i1 %empty, label %done, label %loop
loop:
  %g = phi i16 [ 0, %entry ], [ %g.next, %loop ]
  %i = shl i16 %g, 2
  %pi = getelementptr <4 x float>, ptr inttoptr (i16 16 to ptr), i16 %i
  %g3 = mul i16 %g, 3
  %o = add i16 %g3, %n
  %o1 = add i16 %o, 1
  %po = getelementptr <4 x float>, ptr inttoptr (i16 16 to ptr), i16 %o1
  %x = load <4 x float>, ptr %pi
  %y = fadd <4 x float> %x, %x
  store <4 x float> %y, ptr %po
  %g.next = add i16 %g, 1
  %c = icmp eq i16 %g.next, %m
  br i1 %c, label %done, label %loop
done:
  ret void
}

; The store of iteration g is the load of iteration g + 1: no scopes.
; CHECK-LABEL: define void @overlap(
; CHECK: load <4 x float>, ptr %pi, align 16{{$}}
; CHECK: store <4 x float> %y, ptr %po, align 16{{$}}
define void @overlap() {
entry:
  br label %loop
loop:
  %i = phi i16 [ 0, %entry ], [ %i.next, %loop ]
  %pi = getelementptr <4 x float>, ptr inttoptr (i16 16 to ptr), i16 %i
  %i.next = add i16 %i, 1
  %po = getelementptr <4 x float>, ptr inttoptr (i16 16 to ptr), i16 %i.next
  %x = load <4 x float>, ptr %pi
  %y = fadd <4 x float> %x, %x
  store <4 x float> %y, ptr %po
  %c = icmp eq i16 %i.next, 64
  br i1 %c, label %done, label %loop
done:
  ret void
}
