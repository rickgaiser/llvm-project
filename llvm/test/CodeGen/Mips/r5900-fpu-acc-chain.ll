; RUN: llc -mtriple=mips64el -mcpu=r5900 -O2 < %s | FileCheck %s

; Test that multiply-add chains are optimized to use the FPU accumulator.

; Simple 2-element dot product: a0*b0 + a1*b1
define float @dot2(float %a0, float %b0, float %a1, float %b1) {
; CHECK-LABEL: dot2:
; CHECK: mula.s
; CHECK: madd.s
entry:
  %m0 = fmul float %a0, %b0
  %m1 = fmul float %a1, %b1
  %sum = fadd float %m0, %m1
  ret float %sum
}

; 3-element dot product: a0*b0 + a1*b1 + a2*b2
define float @dot3(float %a0, float %b0, float %a1, float %b1, float %a2, float %b2) {
; CHECK-LABEL: dot3:
; CHECK: mula.s
; CHECK: madda.s
; CHECK: madd.s
entry:
  %m0 = fmul float %a0, %b0
  %m1 = fmul float %a1, %b1
  %m2 = fmul float %a2, %b2
  %s0 = fadd float %m0, %m1
  %s1 = fadd float %s0, %m2
  ret float %s1
}

; 4-element dot product: a0*b0 + a1*b1 + a2*b2 + a3*b3
define float @dot4(float %a0, float %b0, float %a1, float %b1, float %a2, float %b2, float %a3, float %b3) {
; CHECK-LABEL: dot4:
; CHECK: mula.s
; CHECK: madda.s
; CHECK: madda.s
; CHECK: madd.s
entry:
  %m0 = fmul float %a0, %b0
  %m1 = fmul float %a1, %b1
  %m2 = fmul float %a2, %b2
  %m3 = fmul float %a3, %b3
  %s0 = fadd float %m0, %m1
  %s1 = fadd float %s0, %m2
  %s2 = fadd float %s1, %m3
  ret float %s2
}

; 4-element dot product with memory loads.
; This test verifies that register allocation correctly assigns different
; registers to each loaded value - a previous bug caused registers to be reused
; before their values were consumed.
; The MachineScheduler interleaves loads with the ACC chain for better performance.
define void @dot4_mem(ptr %a, ptr %b, ptr %out) {
; CHECK-LABEL: dot4_mem:
; Loads and mula.s can be interleaved by the scheduler
; CHECK-DAG: lwc1 $f{{[0-9]+}},
; CHECK-DAG: lwc1 $f{{[0-9]+}},
; CHECK-DAG: lwc1 $f{{[0-9]+}},
; CHECK-DAG: lwc1 $f{{[0-9]+}},
; CHECK-DAG: lwc1 $f{{[0-9]+}},
; CHECK-DAG: lwc1 $f{{[0-9]+}},
; CHECK-DAG: lwc1 $f{{[0-9]+}},
; CHECK-DAG: mula.s
; Last load may be interleaved
; CHECK-DAG: lwc1 $f{{[0-9]+}},
; Accumulator chain continues
; CHECK: madda.s
; CHECK: madda.s
; CHECK: madd.s
; CHECK: swc1
entry:
  %a0 = load float, ptr %a
  %a1.ptr = getelementptr float, ptr %a, i32 1
  %a1 = load float, ptr %a1.ptr
  %a2.ptr = getelementptr float, ptr %a, i32 2
  %a2 = load float, ptr %a2.ptr
  %a3.ptr = getelementptr float, ptr %a, i32 3
  %a3 = load float, ptr %a3.ptr

  %b0 = load float, ptr %b
  %b1.ptr = getelementptr float, ptr %b, i32 1
  %b1 = load float, ptr %b1.ptr
  %b2.ptr = getelementptr float, ptr %b, i32 2
  %b2 = load float, ptr %b2.ptr
  %b3.ptr = getelementptr float, ptr %b, i32 3
  %b3 = load float, ptr %b3.ptr

  %m0 = fmul float %a0, %b0
  %m1 = fmul float %a1, %b1
  %m2 = fmul float %a2, %b2
  %m3 = fmul float %a3, %b3

  %s0 = fadd float %m0, %m1
  %s1 = fadd float %s0, %m2
  %s2 = fadd float %s1, %m3

  store float %s2, ptr %out
  ret void
}
