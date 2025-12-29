; RUN: llc -mtriple=dvpvu < %s | FileCheck %s
; Test division operations (uses DIV + WAITQ + Q sequence)

; XFAIL: *
; TODO: Implement proper FDIV lowering

; Division on VU uses a multi-instruction sequence:
; 1. DIV q, src1, src2  - starts the division
; 2. WAITQ             - waits for Q register to be ready
; 3. Use Q register result

; CHECK-LABEL: test_fdiv:
; CHECK: div
; CHECK: waitq
define <4 x float> @test_fdiv(<4 x float> %a, <4 x float> %b) {
  %result = fdiv <4 x float> %a, %b
  ret <4 x float> %result
}
