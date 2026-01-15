; RUN: llc -mtriple=mips64el -mcpu=r5900 < %s | FileCheck %s
;
; Test basic R5900 (PlayStation 2 Emotion Engine) code generation.
; R5900 is based on MIPS III with extensions:
; - Single-precision FPU only (no double precision)
; - 128-bit GPR extensions (not tested here yet)
; - No LL/SC atomic instructions

; CHECK-LABEL: test_add:
; CHECK: daddu
define i64 @test_add(i64 %a, i64 %b) {
  %c = add i64 %a, %b
  ret i64 %c
}

; Test 64-bit multiply (R5900 has no dmult, so it expands to 32-bit mults)
; CHECK-LABEL: test_mul:
; CHECK: mult
; CHECK: multu
define i64 @test_mul(i64 %a, i64 %b) {
  %c = mul i64 %a, %b
  ret i64 %c
}

; Test single-precision float (should work)
; CHECK-LABEL: test_float_add:
; CHECK: add.s
define float @test_float_add(float %a, float %b) {
  %c = fadd float %a, %b
  ret float %c
}

; CHECK-LABEL: test_float_mul:
; CHECK: mul.s
define float @test_float_mul(float %a, float %b) {
  %c = fmul float %a, %b
  ret float %c
}

; Test 32-bit operations
; CHECK-LABEL: test_add32:
; CHECK: addu
define i32 @test_add32(i32 %a, i32 %b) {
  %c = add i32 %a, %b
  ret i32 %c
}
