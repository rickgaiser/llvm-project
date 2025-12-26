; RUN: llc -mtriple=mips64el -mcpu=r5900 < %s | FileCheck %s

; Test that R5900 FPU min/max instructions are generated for llvm.maxnum/minnum

define float @test_max(float %a, float %b) {
; CHECK-LABEL: test_max:
; CHECK: max.s
  %r = call float @llvm.maxnum.f32(float %a, float %b)
  ret float %r
}

define float @test_min(float %a, float %b) {
; CHECK-LABEL: test_min:
; CHECK: min.s
  %r = call float @llvm.minnum.f32(float %a, float %b)
  ret float %r
}

declare float @llvm.maxnum.f32(float, float)
declare float @llvm.minnum.f32(float, float)
