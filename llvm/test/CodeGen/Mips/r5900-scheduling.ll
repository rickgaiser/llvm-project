; RUN: llc -mtriple=mips64el-none-elf -mcpu=r5900 -O2 < %s | FileCheck %s

; Test that FPU operations are properly scheduled on R5900.
; The R5900 has a 2-way superscalar pipeline where loads (LS pipe) can
; dual-issue with FPU operations (C1 pipe).

; This test uses the R5900 FPU accumulator chain (mula.s/madda.s/madd.s)
; for dot products, which is automatically generated.
define void @dot_product_interleave(ptr %a, ptr %b, ptr %out) {
; CHECK-LABEL: dot_product_interleave:
; CHECK: lwc1
; CHECK: lwc1
; R5900 uses accumulator instructions for dot products
; CHECK: mula.s
; CHECK: madda.s
; CHECK: madd.s
; CHECK: swc1
  %a0.ptr = getelementptr float, ptr %a, i32 0
  %a1.ptr = getelementptr float, ptr %a, i32 1
  %a2.ptr = getelementptr float, ptr %a, i32 2
  %a3.ptr = getelementptr float, ptr %a, i32 3

  %b0.ptr = getelementptr float, ptr %b, i32 0
  %b1.ptr = getelementptr float, ptr %b, i32 1
  %b2.ptr = getelementptr float, ptr %b, i32 2
  %b3.ptr = getelementptr float, ptr %b, i32 3

  %a0 = load float, ptr %a0.ptr
  %a1 = load float, ptr %a1.ptr
  %a2 = load float, ptr %a2.ptr
  %a3 = load float, ptr %a3.ptr

  %b0 = load float, ptr %b0.ptr
  %b1 = load float, ptr %b1.ptr
  %b2 = load float, ptr %b2.ptr
  %b3 = load float, ptr %b3.ptr

  ; Compute dot product: a0*b0 + a1*b1 + a2*b2 + a3*b3
  %m0 = fmul float %a0, %b0
  %m1 = fmul float %a1, %b1
  %m2 = fmul float %a2, %b2
  %m3 = fmul float %a3, %b3

  %s0 = fadd float %m0, %m1
  %s1 = fadd float %m2, %m3
  %result = fadd float %s0, %s1

  store float %result, ptr %out
  ret void
}

; Test: two independent multiplies
; The MachineScheduler loads all values first, then performs multiplies.
define void @two_muls(ptr %a, ptr %b, ptr %c, ptr %d, ptr %out1, ptr %out2) {
; CHECK-LABEL: two_muls:
; All loads first
; CHECK: lwc1
; CHECK: lwc1
; CHECK: lwc1
; CHECK: lwc1
; Then both multiplies
; CHECK: mul.s
; CHECK: mul.s
; Then stores
; CHECK: swc1
; CHECK: swc1
  %va = load float, ptr %a
  %vb = load float, ptr %b
  %vc = load float, ptr %c
  %vd = load float, ptr %d

  %m1 = fmul float %va, %vb
  %m2 = fmul float %vc, %vd

  store float %m1, ptr %out1
  store float %m2, ptr %out2
  ret void
}
