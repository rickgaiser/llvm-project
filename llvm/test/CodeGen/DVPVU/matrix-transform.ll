; RUN: llc -mtriple=dvpvu < %s | FileCheck %s

; Test matrix-vector multiplication with 5 vector arguments
; This exercises the expanded calling convention (VF4-VF15)
; Matrix (4x4) x Vector (4-component) = m0*v.x + m1*v.y + m2*v.z + m3*v.w

define <4 x float> @matrix_transform(<4 x float> %m0, <4 x float> %m1,
                                      <4 x float> %m2, <4 x float> %m3,
                                      <4 x float> %v) {
; CHECK-LABEL: matrix_transform:
; CHECK: mulx.xyzw
; CHECK: muly.xyzw
; CHECK: mulz.xyzw
; CHECK: mulw.xyzw
; CHECK: add.xyzw
entry:
  ; Broadcast each component of v
  %v0 = shufflevector <4 x float> %v, <4 x float> poison, <4 x i32> zeroinitializer
  %v1 = shufflevector <4 x float> %v, <4 x float> poison, <4 x i32> <i32 1, i32 1, i32 1, i32 1>
  %v2 = shufflevector <4 x float> %v, <4 x float> poison, <4 x i32> <i32 2, i32 2, i32 2, i32 2>
  %v3 = shufflevector <4 x float> %v, <4 x float> poison, <4 x i32> <i32 3, i32 3, i32 3, i32 3>

  ; Multiply each matrix row by the broadcasted component
  %t0 = fmul <4 x float> %m0, %v0
  %t1 = fmul <4 x float> %m1, %v1
  %t2 = fmul <4 x float> %m2, %v2
  %t3 = fmul <4 x float> %m3, %v3

  ; Accumulate results
  %a0 = fadd <4 x float> %t0, %t1
  %a1 = fadd <4 x float> %t2, %t3
  %result = fadd <4 x float> %a0, %a1

  ret <4 x float> %result
}

; Test with 8 vector arguments to verify full expanded convention
define <4 x float> @eight_vector_args(<4 x float> %a0, <4 x float> %a1,
                                       <4 x float> %a2, <4 x float> %a3,
                                       <4 x float> %a4, <4 x float> %a5,
                                       <4 x float> %a6, <4 x float> %a7) {
; CHECK-LABEL: eight_vector_args:
; CHECK: add.xyzw
entry:
  %s0 = fadd <4 x float> %a0, %a1
  %s1 = fadd <4 x float> %a2, %a3
  %s2 = fadd <4 x float> %a4, %a5
  %s3 = fadd <4 x float> %a6, %a7
  %t0 = fadd <4 x float> %s0, %s1
  %t1 = fadd <4 x float> %s2, %s3
  %result = fadd <4 x float> %t0, %t1
  ret <4 x float> %result
}

; Test with 12 vector arguments (maximum in convention)
define <4 x float> @twelve_vector_args(<4 x float> %a0, <4 x float> %a1,
                                        <4 x float> %a2, <4 x float> %a3,
                                        <4 x float> %a4, <4 x float> %a5,
                                        <4 x float> %a6, <4 x float> %a7,
                                        <4 x float> %a8, <4 x float> %a9,
                                        <4 x float> %a10, <4 x float> %a11) {
; CHECK-LABEL: twelve_vector_args:
; CHECK: add.xyzw
entry:
  %s0 = fadd <4 x float> %a0, %a1
  %s1 = fadd <4 x float> %a2, %a3
  %s2 = fadd <4 x float> %a4, %a5
  %s3 = fadd <4 x float> %a6, %a7
  %s4 = fadd <4 x float> %a8, %a9
  %s5 = fadd <4 x float> %a10, %a11
  %t0 = fadd <4 x float> %s0, %s1
  %t1 = fadd <4 x float> %s2, %s3
  %t2 = fadd <4 x float> %s4, %s5
  %u0 = fadd <4 x float> %t0, %t1
  %result = fadd <4 x float> %u0, %t2
  ret <4 x float> %result
}
