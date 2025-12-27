; RUN: llc -mtriple=mips64el -mcpu=r5900 < %s | FileCheck %s

; Test that R5900 uses MADD for multiply-add chains

; Simple multiply then add - no MADD optimization (would need mtlo first)
; CHECK-LABEL: test_simple_mul_add:
; CHECK: mult ${{[0-9]+}}, ${{[0-9]+}}, ${{[0-9]+}}
; CHECK: addu
define i32 @test_simple_mul_add(i32 %a, i32 %b, i32 %c) {
  %mul = mul i32 %a, %b
  %add = add i32 %mul, %c
  ret i32 %add
}

; Two multiplies added together - uses MULT + MADD chain
; CHECK-LABEL: test_chain_madd:
; CHECK: mult ${{[0-9]+}}, ${{[0-9]+}}, ${{[0-9]+}}
; CHECK: madd ${{[0-9]+}}, ${{[0-9]+}}, ${{[0-9]+}}
; CHECK-NOT: addu
define i32 @test_chain_madd(i32 %a, i32 %b, i32 %c, i32 %d) {
  %mul1 = mul i32 %a, %b
  %mul2 = mul i32 %c, %d
  %add = add i32 %mul1, %mul2
  ret i32 %add
}

; Commuted version - should also use MADD
; CHECK-LABEL: test_chain_madd_commuted:
; CHECK: mult ${{[0-9]+}}, ${{[0-9]+}}, ${{[0-9]+}}
; CHECK: madd ${{[0-9]+}}, ${{[0-9]+}}, ${{[0-9]+}}
; CHECK-NOT: addu
define i32 @test_chain_madd_commuted(i32 %a, i32 %b, i32 %c, i32 %d) {
  %mul1 = mul i32 %a, %b
  %mul2 = mul i32 %c, %d
  %add = add i32 %mul2, %mul1
  ret i32 %add
}

; Three multiplies - at least one MADD should be used
; CHECK-LABEL: test_triple_madd:
; CHECK: mult
; CHECK: madd
define i32 @test_triple_madd(i32 %a, i32 %b, i32 %c, i32 %d, i32 %e, i32 %f) {
  %mul1 = mul i32 %a, %b
  %mul2 = mul i32 %c, %d
  %mul3 = mul i32 %e, %f
  %add1 = add i32 %mul1, %mul2
  %add2 = add i32 %add1, %mul3
  ret i32 %add2
}
