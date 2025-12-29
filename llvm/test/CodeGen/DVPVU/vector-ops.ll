; RUN: llc -mtriple=dvpvu < %s | FileCheck %s
; Test basic vector operations on VU v4f32 type

; XFAIL: *
; TODO: Add instruction selection patterns for vector ops

; CHECK-LABEL: test_vadd:
; CHECK: add.xyzw vf{{[0-9]+}}, vf{{[0-9]+}}, vf{{[0-9]+}}
define <4 x float> @test_vadd(<4 x float> %a, <4 x float> %b) {
  %result = fadd <4 x float> %a, %b
  ret <4 x float> %result
}

; CHECK-LABEL: test_vsub:
; CHECK: sub.xyzw vf{{[0-9]+}}, vf{{[0-9]+}}, vf{{[0-9]+}}
define <4 x float> @test_vsub(<4 x float> %a, <4 x float> %b) {
  %result = fsub <4 x float> %a, %b
  ret <4 x float> %result
}

; CHECK-LABEL: test_vmul:
; CHECK: mul.xyzw vf{{[0-9]+}}, vf{{[0-9]+}}, vf{{[0-9]+}}
define <4 x float> @test_vmul(<4 x float> %a, <4 x float> %b) {
  %result = fmul <4 x float> %a, %b
  ret <4 x float> %result
}
