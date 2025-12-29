; RUN: llc -mtriple=dvpvu < %s | FileCheck %s

; Test broadcast multiply operations
; These patterns are used for matrix-vector multiplication:
; result = m0 * v.x + m1 * v.y + m2 * v.z + m3 * v.w

; Test broadcast x component multiply
define <4 x float> @test_mulx(<4 x float> %m, <4 x float> %v) {
; CHECK-LABEL: test_mulx:
; CHECK: mulx.xyzw
  %v0 = shufflevector <4 x float> %v, <4 x float> poison, <4 x i32> zeroinitializer
  %result = fmul <4 x float> %m, %v0
  ret <4 x float> %result
}

; Test broadcast y component multiply
define <4 x float> @test_muly(<4 x float> %m, <4 x float> %v) {
; CHECK-LABEL: test_muly:
; CHECK: muly.xyzw
  %v1 = shufflevector <4 x float> %v, <4 x float> poison, <4 x i32> <i32 1, i32 1, i32 1, i32 1>
  %result = fmul <4 x float> %m, %v1
  ret <4 x float> %result
}

; Test broadcast z component multiply
define <4 x float> @test_mulz(<4 x float> %m, <4 x float> %v) {
; CHECK-LABEL: test_mulz:
; CHECK: mulz.xyzw
  %v2 = shufflevector <4 x float> %v, <4 x float> poison, <4 x i32> <i32 2, i32 2, i32 2, i32 2>
  %result = fmul <4 x float> %m, %v2
  ret <4 x float> %result
}

; Test broadcast w component multiply
define <4 x float> @test_mulw(<4 x float> %m, <4 x float> %v) {
; CHECK-LABEL: test_mulw:
; CHECK: mulw.xyzw
  %v3 = shufflevector <4 x float> %v, <4 x float> poison, <4 x i32> <i32 3, i32 3, i32 3, i32 3>
  %result = fmul <4 x float> %m, %v3
  ret <4 x float> %result
}

; Test all four broadcast components in one function
define <4 x float> @test_all_broadcasts(<4 x float> %m, <4 x float> %v) {
; CHECK-LABEL: test_all_broadcasts:
; CHECK: mulx.xyzw
; CHECK: muly.xyzw
; CHECK: mulz.xyzw
; CHECK: mulw.xyzw
; CHECK: add.xyzw
  %v0 = shufflevector <4 x float> %v, <4 x float> poison, <4 x i32> zeroinitializer
  %v1 = shufflevector <4 x float> %v, <4 x float> poison, <4 x i32> <i32 1, i32 1, i32 1, i32 1>
  %v2 = shufflevector <4 x float> %v, <4 x float> poison, <4 x i32> <i32 2, i32 2, i32 2, i32 2>
  %v3 = shufflevector <4 x float> %v, <4 x float> poison, <4 x i32> <i32 3, i32 3, i32 3, i32 3>

  %t0 = fmul <4 x float> %m, %v0
  %t1 = fmul <4 x float> %m, %v1
  %t2 = fmul <4 x float> %m, %v2
  %t3 = fmul <4 x float> %m, %v3

  %a0 = fadd <4 x float> %t0, %t1
  %a1 = fadd <4 x float> %t2, %t3
  %result = fadd <4 x float> %a0, %a1

  ret <4 x float> %result
}
