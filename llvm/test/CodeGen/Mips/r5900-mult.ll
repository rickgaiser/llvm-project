; RUN: llc -mtriple=mips64el -mcpu=r5900 < %s | FileCheck %s

; Test that R5900 uses 3-operand mult instead of mult + mflo

; CHECK-LABEL: test_mult_i32:
; CHECK: mult ${{[0-9]+}}, ${{[0-9]+}}, ${{[0-9]+}}
; CHECK-NOT: mflo
define i32 @test_mult_i32(i32 %a, i32 %b) {
  %result = mul i32 %a, %b
  ret i32 %result
}

; CHECK-LABEL: test_mult_i32_add:
; CHECK: mult ${{[0-9]+}}, ${{[0-9]+}}, ${{[0-9]+}}
; CHECK-NOT: mflo
define i32 @test_mult_i32_add(i32 %a, i32 %b, i32 %c) {
  %mul = mul i32 %a, %b
  %add = add i32 %mul, %c
  ret i32 %add
}

; CHECK-LABEL: test_mult_i32_sub:
; CHECK: mult ${{[0-9]+}}, ${{[0-9]+}}, ${{[0-9]+}}
; CHECK-NOT: mflo
define i32 @test_mult_i32_sub(i32 %a, i32 %b, i32 %c) {
  %mul = mul i32 %a, %b
  %sub = sub i32 %c, %mul
  ret i32 %sub
}

; Test multiple multiplies - should all use 3-operand form
; CHECK-LABEL: test_mult_chain:
; CHECK: mult ${{[0-9]+}}, ${{[0-9]+}}, ${{[0-9]+}}
; CHECK: mult ${{[0-9]+}}, ${{[0-9]+}}, ${{[0-9]+}}
; CHECK-NOT: mflo
define i32 @test_mult_chain(i32 %a, i32 %b, i32 %c) {
  %mul1 = mul i32 %a, %b
  %mul2 = mul i32 %mul1, %c
  ret i32 %mul2
}
