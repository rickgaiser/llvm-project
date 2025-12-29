; RUN: llc -mtriple=dvpvu < %s | FileCheck %s

; Test basic vector operations

; CHECK-LABEL: test_fadd:
; CHECK: add.xyzw
define <4 x float> @test_fadd(<4 x float> %a, <4 x float> %b) {
  %r = fadd <4 x float> %a, %b
  ret <4 x float> %r
}

; CHECK-LABEL: test_fsub:
; CHECK: sub.xyzw
define <4 x float> @test_fsub(<4 x float> %a, <4 x float> %b) {
  %r = fsub <4 x float> %a, %b
  ret <4 x float> %r
}

; CHECK-LABEL: test_fmul:
; CHECK: mul.xyzw
define <4 x float> @test_fmul(<4 x float> %a, <4 x float> %b) {
  %r = fmul <4 x float> %a, %b
  ret <4 x float> %r
}

; CHECK-LABEL: test_iadd:
; CHECK: iadd
define i16 @test_iadd(i16 %a, i16 %b) {
  %r = add i16 %a, %b
  ret i16 %r
}

; CHECK-LABEL: test_madd:
; CHECK: mul.xyzw
; CHECK: add.xyzw
define <4 x float> @test_madd(<4 x float> %a, <4 x float> %b, <4 x float> %c) {
  %t = fmul <4 x float> %a, %b
  %r = fadd <4 x float> %t, %c
  ret <4 x float> %r
}
