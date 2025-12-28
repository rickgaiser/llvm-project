; RUN: llc -mtriple=mips64el -mcpu=r5900 -O2 < %s | FileCheck %s

; Diagnostic test for R5900 instruction scheduling
; This test analyzes dual-issue behavior for common patterns

; Test 4: Scalar multiply loop (key test case for scheduling)
; Pattern: load + multiply + store per iteration
; Expected: loads should interleave with multiplies for dual-issue
define void @int_scalar_multiply(ptr noalias %in, ptr noalias %out, i32 %count, i32 %scalar) {
; CHECK-LABEL: int_scalar_multiply:
; The loop body should have interleaved loads/mults/stores
; CHECK: .LBB{{[0-9]+}}_{{[0-9]+}}:
; CHECK: lw
; CHECK: mult
; CHECK: sw
entry:
  %cmp = icmp sgt i32 %count, 0
  br i1 %cmp, label %for.body.preheader, label %for.end

for.body.preheader:
  br label %for.body

for.body:
  %i = phi i32 [ %inc, %for.body ], [ 0, %for.body.preheader ]
  %idxprom = sext i32 %i to i64
  %arrayidx.in = getelementptr inbounds i32, ptr %in, i64 %idxprom
  %val = load i32, ptr %arrayidx.in, align 4
  %mul = mul i32 %val, %scalar
  %arrayidx.out = getelementptr inbounds i32, ptr %out, i64 %idxprom
  store i32 %mul, ptr %arrayidx.out, align 4
  %inc = add nuw nsw i32 %i, 1
  %exitcond = icmp eq i32 %inc, %count
  br i1 %exitcond, label %for.end, label %for.body

for.end:
  ret void
}

; Unrolled version (4x) to see scheduling across multiple iterations
define void @int_scalar_multiply_unroll4(ptr noalias %in, ptr noalias %out, i32 %scalar) {
; CHECK-LABEL: int_scalar_multiply_unroll4:
; With unrolling, scheduler should interleave multiple load/mult/store chains
; CHECK: lw
; CHECK: lw
; CHECK: mult
; CHECK: mult
entry:
  ; Iteration 0
  %val0 = load i32, ptr %in, align 4
  %mul0 = mul i32 %val0, %scalar
  store i32 %mul0, ptr %out, align 4

  ; Iteration 1
  %in1 = getelementptr inbounds i32, ptr %in, i64 1
  %out1 = getelementptr inbounds i32, ptr %out, i64 1
  %val1 = load i32, ptr %in1, align 4
  %mul1 = mul i32 %val1, %scalar
  store i32 %mul1, ptr %out1, align 4

  ; Iteration 2
  %in2 = getelementptr inbounds i32, ptr %in, i64 2
  %out2 = getelementptr inbounds i32, ptr %out, i64 2
  %val2 = load i32, ptr %in2, align 4
  %mul2 = mul i32 %val2, %scalar
  store i32 %mul2, ptr %out2, align 4

  ; Iteration 3
  %in3 = getelementptr inbounds i32, ptr %in, i64 3
  %out3 = getelementptr inbounds i32, ptr %out, i64 3
  %val3 = load i32, ptr %in3, align 4
  %mul3 = mul i32 %val3, %scalar
  store i32 %mul3, ptr %out3, align 4

  ret void
}
