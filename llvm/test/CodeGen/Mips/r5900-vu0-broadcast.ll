; RUN: llc -mtriple=mips64el -mcpu=r5900 -mattr=+vu0 < %s | FileCheck %s

; Test that VU0 broadcast multiply instructions are generated from
; shufflevector + fmul patterns. This enables efficient matrix-vector
; multiplication using VMULbc instructions.

; Test broadcast multiply from x component (element 0)
; CHECK-LABEL: broadcast_mul_x:
; CHECK: vmulx.xyzw
define <4 x float> @broadcast_mul_x(<4 x float> %a, <4 x float> %b) {
  %splat = shufflevector <4 x float> %b, <4 x float> undef, <4 x i32> zeroinitializer
  %result = fmul <4 x float> %a, %splat
  ret <4 x float> %result
}

; Test broadcast multiply from y component (element 1)
; CHECK-LABEL: broadcast_mul_y:
; CHECK: vmuly.xyzw
define <4 x float> @broadcast_mul_y(<4 x float> %a, <4 x float> %b) {
  %splat = shufflevector <4 x float> %b, <4 x float> undef, <4 x i32> <i32 1, i32 1, i32 1, i32 1>
  %result = fmul <4 x float> %a, %splat
  ret <4 x float> %result
}

; Test broadcast multiply from z component (element 2)
; CHECK-LABEL: broadcast_mul_z:
; CHECK: vmulz.xyzw
define <4 x float> @broadcast_mul_z(<4 x float> %a, <4 x float> %b) {
  %splat = shufflevector <4 x float> %b, <4 x float> undef, <4 x i32> <i32 2, i32 2, i32 2, i32 2>
  %result = fmul <4 x float> %a, %splat
  ret <4 x float> %result
}

; Test broadcast multiply from w component (element 3)
; CHECK-LABEL: broadcast_mul_w:
; CHECK: vmulw.xyzw
define <4 x float> @broadcast_mul_w(<4 x float> %a, <4 x float> %b) {
  %splat = shufflevector <4 x float> %b, <4 x float> undef, <4 x i32> <i32 3, i32 3, i32 3, i32 3>
  %result = fmul <4 x float> %a, %splat
  ret <4 x float> %result
}

; Test broadcast multiply with operands swapped (fmul is commutative)
; CHECK-LABEL: broadcast_mul_commutative:
; CHECK: vmulx.xyzw
define <4 x float> @broadcast_mul_commutative(<4 x float> %a, <4 x float> %b) {
  %splat = shufflevector <4 x float> %b, <4 x float> undef, <4 x i32> zeroinitializer
  %result = fmul <4 x float> %splat, %a
  ret <4 x float> %result
}

; Test broadcast multiply with undef elements in shuffle mask
; Should still match as broadcast from element 2
; CHECK-LABEL: broadcast_mul_with_undef:
; CHECK: vmulz.xyzw
define <4 x float> @broadcast_mul_with_undef(<4 x float> %a, <4 x float> %b) {
  %splat = shufflevector <4 x float> %b, <4 x float> undef, <4 x i32> <i32 2, i32 undef, i32 2, i32 2>
  %result = fmul <4 x float> %a, %splat
  ret <4 x float> %result
}

; Test matrix-vector multiplication pattern with ACC chain optimization
; result = m0*v.x + m1*v.y + m2*v.z + m3*v.w
; The ACC chain optimization pass transforms the broadcast multiply chain
; from VMULbc + VADD sequences into VMULA/VMADD accumulator chains.
;
; CHECK-LABEL: matvec_simple:
; CHECK: vmulax.xyzw
; CHECK: vmadday.xyzw
; CHECK: vmaddaz.xyzw
; CHECK: vmaddw.xyzw
define <4 x float> @matvec_simple(<4 x float> %m0, <4 x float> %m1,
                                   <4 x float> %m2, <4 x float> %m3,
                                   <4 x float> %v) {
  %vx = shufflevector <4 x float> %v, <4 x float> undef, <4 x i32> zeroinitializer
  %vy = shufflevector <4 x float> %v, <4 x float> undef, <4 x i32> <i32 1, i32 1, i32 1, i32 1>
  %vz = shufflevector <4 x float> %v, <4 x float> undef, <4 x i32> <i32 2, i32 2, i32 2, i32 2>
  %vw = shufflevector <4 x float> %v, <4 x float> undef, <4 x i32> <i32 3, i32 3, i32 3, i32 3>

  %t0 = fmul <4 x float> %m0, %vx
  %t1 = fmul <4 x float> %m1, %vy
  %t2 = fmul <4 x float> %m2, %vz
  %t3 = fmul <4 x float> %m3, %vw

  %s01 = fadd <4 x float> %t0, %t1
  %s012 = fadd <4 x float> %s01, %t2
  %result = fadd <4 x float> %s012, %t3

  ret <4 x float> %result
}

; Test 2-element chain (minimum for ACC optimization)
; CHECK-LABEL: matvec_2elem:
; CHECK: vmulax.xyzw
; CHECK: vmaddy.xyzw
define <4 x float> @matvec_2elem(<4 x float> %m0, <4 x float> %m1, <4 x float> %v) {
  %vx = shufflevector <4 x float> %v, <4 x float> undef, <4 x i32> zeroinitializer
  %vy = shufflevector <4 x float> %v, <4 x float> undef, <4 x i32> <i32 1, i32 1, i32 1, i32 1>

  %t0 = fmul <4 x float> %m0, %vx
  %t1 = fmul <4 x float> %m1, %vy

  %result = fadd <4 x float> %t0, %t1

  ret <4 x float> %result
}
