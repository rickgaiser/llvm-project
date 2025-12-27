; RUN: llc -mtriple=mips64el -mcpu=r5900 -mips-r5900-pipeline-balance < %s | FileCheck %s

; Test that R5900 pipeline balancer converts independent multiplies to Pipeline 1

; The (add (mul a, b), (mul c, d)) pattern is already handled by PseudoR5900MulMulAdd
; which emits MULT + MADD on Pipeline 0. The balancer correctly preserves this chain.
; CHECK-LABEL: test_madd_chain:
; CHECK: mult ${{[0-9]+}}, ${{[0-9]+}}, ${{[0-9]+}}
; CHECK: madd ${{[0-9]+}}, ${{[0-9]+}}, ${{[0-9]+}}
; CHECK-NOT: mult1
define i32 @test_madd_chain(i32 %a, i32 %b, i32 %c, i32 %d) {
  %mul1 = mul i32 %a, %b
  %mul2 = mul i32 %c, %d
  %sum = add i32 %mul1, %mul2
  ret i32 %sum
}

; Three multiplies where one is independent of the MADD chain.
; The independent multiply can use Pipeline 1.
; CHECK-LABEL: test_mixed_pipeline:
; CHECK-DAG: mult1 ${{[0-9]+}}, ${{[0-9]+}}, ${{[0-9]+}}
; CHECK-DAG: mult ${{[0-9]+}}, ${{[0-9]+}}, ${{[0-9]+}}
; CHECK: madd ${{[0-9]+}}, ${{[0-9]+}}, ${{[0-9]+}}
define i32 @test_mixed_pipeline(i32 %a, i32 %b, i32 %c, i32 %d, i32 %e, i32 %f) {
  %mul1 = mul i32 %a, %b
  %mul2 = mul i32 %c, %d
  %mul3 = mul i32 %e, %f   ; Independent multiply
  %sum = add i32 %mul1, %mul2  ; Uses MULT+MADD
  %result = add i32 %sum, %mul3
  ret i32 %result
}

; Single multiply - can use either pipeline (pass converts to Pipeline 1)
; CHECK-LABEL: test_single_mult:
; CHECK: mult
define i32 @test_single_mult(i32 %a, i32 %b) {
  %mul = mul i32 %a, %b
  ret i32 %mul
}

; Two completely separate multiplies that are both returned
; Both can use different pipelines for parallelism
; CHECK-LABEL: test_separate_mults:
; CHECK: mult
define { i32, i32 } @test_separate_mults(i32 %a, i32 %b, i32 %c, i32 %d) {
  %mul1 = mul i32 %a, %b
  %mul2 = mul i32 %c, %d
  %r1 = insertvalue { i32, i32 } undef, i32 %mul1, 0
  %r2 = insertvalue { i32, i32 } %r1, i32 %mul2, 1
  ret { i32, i32 } %r2
}

; Test without the flag - should NOT use Pipeline 1
; RUN: llc -mtriple=mips64el -mcpu=r5900 < %s | FileCheck %s --check-prefix=NOFLAG
; NOFLAG-LABEL: test_madd_chain:
; NOFLAG-NOT: mult1
