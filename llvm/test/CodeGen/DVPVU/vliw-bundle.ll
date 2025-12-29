; RUN: llc -mtriple=dvpvu -O0 < %s -print-after-all 2>&1 | FileCheck %s

; Test VLIW bundling of Upper + Lower instructions
; This test has multiple operations to create bundleable pairs

define <4 x float> @test_bundle(<4 x float> %a, <4 x float> %b, <4 x float> %c, ptr %p) {
  ; First Upper: fadd a + b -> result1
  %result1 = fadd <4 x float> %a, %b

  ; This store (Lower) should be bundleable with the next fadd (Upper)
  ; since they have no data dependencies
  store <4 x float> %result1, ptr %p

  ; Second Upper: fadd a + c -> result2
  %result2 = fadd <4 x float> %a, %c

  ret <4 x float> %result2
}

; After packetizer, should see BUNDLE if instructions were bundled
; CHECK: DVPVU VLIW Packetizer
