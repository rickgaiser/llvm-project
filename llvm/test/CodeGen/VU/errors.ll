; RUN: split-file %s %t
; RUN: not llc -mtriple=vu %t/args.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=ARGS
; RUN: not llc -mtriple=vu %t/ret.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=RET
; RUN: not llc -mtriple=vu %t/load.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=LOAD
; RUN: not llc -mtriple=vu %t/call.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=CALL
; RUN: not llc -mtriple=vu -O3 %t/call-loop.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=CALL
; RUN: not llc -mtriple=vu -mcpu=vu0 %t/vu1.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=VU1
; RUN: not llc -mtriple=vu %t/mul.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=MUL

; ARGS: error: {{.*}} VU: micro programs take no arguments
; RET:  error: {{.*}} VU: micro programs do not return a value
; LOAD: error: {{.*}} VU: unsupported load type
; LOAD: error: {{.*}} VU: memory access that is not by quadwords
; CALL: error: {{.*}} VU: function calls are not supported
; VU1:  error: {{.*}} VU: XGKICK and XTOP require VU1
; MUL:  error: {{.*}} VU: integer multiply by a variable

;--- args.ll
define void @f(i16 %a) {
  ret void
}

;--- ret.ll
define i16 @f() {
  ret i16 0
}

;--- load.ll
; Bytes, and an index by floats: the VU addresses quadwords.
define void @f(i16 %i) {
  %v = load i8, ptr inttoptr (i16 16 to ptr)
  %p = getelementptr float, ptr inttoptr (i16 32 to ptr), i16 %i
  %f = load float, ptr %p
  ret void
}

;--- call.ll
declare void @g()
define void @f() {
  call void @g()
  ret void
}

;--- call-loop.ll
; A call in a loop: the error, and llc ends (it used to hang).
declare <4 x float> @g(<4 x float>)
declare <4 x float> @llvm.vu.lq(i16)
declare void @llvm.vu.sq(<4 x float>, i16)
define void @f() {
entry:
  br label %loop
loop:
  %i = phi i16 [ 0, %entry ], [ %n, %loop ]
  %v = call <4 x float> @llvm.vu.lq(i16 %i)
  %r = call <4 x float> @g(<4 x float> %v)
  call void @llvm.vu.sq(<4 x float> %r, i16 %i)
  %n = add i16 %i, 1
  %c = icmp eq i16 %n, 10
  br i1 %c, label %done, label %loop
done:
  ret void
}

;--- vu1.ll
declare void @llvm.vu.xgkick(i16)
define void @f() {
  call void @llvm.vu.xgkick(i16 0)
  ret void
}

;--- mul.ll
declare i16 @llvm.vu.ilw(i16, i32)
declare void @llvm.vu.isw(i16, i16, i32)
define void @f() {
  %a = call i16 @llvm.vu.ilw(i16 0, i32 0)
  %b = call i16 @llvm.vu.ilw(i16 1, i32 0)
  %m = mul i16 %a, %b
  call void @llvm.vu.isw(i16 %m, i16 2, i32 0)
  ret void
}
