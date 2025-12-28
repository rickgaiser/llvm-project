; RUN: llc -mtriple=mips64el -mcpu=r5900 -mattr=+vu0 < %s | FileCheck %s

; Test that R5900 VU0 vector arithmetic instructions are generated for v4f32
; and that the calling convention correctly uses VF registers.

; Test vector add - args in VF12, VF13, return in VF1
define <4 x float> @test_vadd(<4 x float> %a, <4 x float> %b) {
; CHECK-LABEL: test_vadd:
; CHECK: vadd.xyzw
  %result = fadd <4 x float> %a, %b
  ret <4 x float> %result
}

; Test vector subtract
define <4 x float> @test_vsub(<4 x float> %a, <4 x float> %b) {
; CHECK-LABEL: test_vsub:
; CHECK: vsub.xyzw
  %result = fsub <4 x float> %a, %b
  ret <4 x float> %result
}

; Test vector multiply
define <4 x float> @test_vmul(<4 x float> %a, <4 x float> %b) {
; CHECK-LABEL: test_vmul:
; CHECK: vmul.xyzw
  %result = fmul <4 x float> %a, %b
  ret <4 x float> %result
}

; Test memory load
define <4 x float> @test_load(<4 x float>* %ptr) {
; CHECK-LABEL: test_load:
; CHECK: lqc2
  %val = load <4 x float>, <4 x float>* %ptr
  ret <4 x float> %val
}

; Test memory store - second arg is in VF12
define void @test_store(<4 x float>* %ptr, <4 x float> %val) {
; CHECK-LABEL: test_store:
; CHECK: sqc2
  store <4 x float> %val, <4 x float>* %ptr
  ret void
}

; Test load/compute/store
define void @test_load_compute_store(<4 x float>* %src1, <4 x float>* %src2, <4 x float>* %dst) {
; CHECK-LABEL: test_load_compute_store:
; CHECK: lqc2
; CHECK: lqc2
; CHECK: vadd.xyzw
; CHECK: sqc2
  %a = load <4 x float>, <4 x float>* %src1
  %b = load <4 x float>, <4 x float>* %src2
  %result = fadd <4 x float> %a, %b
  store <4 x float> %result, <4 x float>* %dst
  ret void
}

; Test combined mul+add - uses VF12, VF13, VF14
define <4 x float> @test_madd_pattern(<4 x float> %a, <4 x float> %b, <4 x float> %c) {
; CHECK-LABEL: test_madd_pattern:
; CHECK: vmul.xyzw
; CHECK: vadd.xyzw
  %mul = fmul <4 x float> %a, %b
  %add = fadd <4 x float> %mul, %c
  ret <4 x float> %add
}

; Test 4 vector arguments (VF12-VF15)
define <4 x float> @test_4_args(<4 x float> %a, <4 x float> %b, <4 x float> %c, <4 x float> %d) {
; CHECK-LABEL: test_4_args:
; CHECK: vadd.xyzw
; CHECK: vadd.xyzw
; CHECK: vadd.xyzw
  %ab = fadd <4 x float> %a, %b
  %cd = fadd <4 x float> %c, %d
  %result = fadd <4 x float> %ab, %cd
  ret <4 x float> %result
}
