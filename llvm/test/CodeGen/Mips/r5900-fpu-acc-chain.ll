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
