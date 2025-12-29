; RUN: llc -mtriple=dvpvu < %s | FileCheck %s
; Test FMAC (fused multiply-add) operations

; XFAIL: *
; TODO: Add FMA patterns

; The VU's FMAC unit is its primary computational resource.
; Key operations:
; - ADD.dest  - vector add
; - SUB.dest  - vector subtract
; - MUL.dest  - vector multiply
; - MADD.dest - multiply-add: ACC + (vs * vt) -> vd, ACC
; - MSUB.dest - multiply-subtract: ACC - (vs * vt) -> vd, ACC
; - MAX.dest  - component-wise maximum
; - MINI.dest - component-wise minimum
; - ABS.dest  - absolute value

; The ACC (accumulator) register is key to MADD/MSUB operations

; CHECK-LABEL: test_fma:
; CHECK: mul
; CHECK: add
define <4 x float> @test_fma(<4 x float> %a, <4 x float> %b, <4 x float> %c) {
  ; c + (a * b)
  %mul = fmul <4 x float> %a, %b
  %result = fadd <4 x float> %c, %mul
  ret <4 x float> %result
}

; This could be optimized to MADD if patterns are added
; CHECK-LABEL: test_fma_contract:
; CHECK: madd
define <4 x float> @test_fma_contract(<4 x float> %a, <4 x float> %b, <4 x float> %c) #0 {
  %mul = fmul contract <4 x float> %a, %b
  %result = fadd contract <4 x float> %c, %mul
  ret <4 x float> %result
}

; CHECK-LABEL: test_fms:
; CHECK: msub
define <4 x float> @test_fms(<4 x float> %a, <4 x float> %b, <4 x float> %c) #0 {
  ; c - (a * b)
  %mul = fmul contract <4 x float> %a, %b
  %result = fsub contract <4 x float> %c, %mul
  ret <4 x float> %result
}

attributes #0 = { "unsafe-fp-math"="true" }
