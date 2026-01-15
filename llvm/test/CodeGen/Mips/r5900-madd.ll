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

;===----------------------------------------------------------------------===;
; 64-bit Accumulator Chain Tests (Widening + Multiple Multiplies)
; R5900 uses PMULTW/PMADDW for widening multiply chains.
; PMULTW: rd = rs[31:0] * rt[31:0] (64-bit result directly in GPR)
; PMADDW: rd = HI:LO + rs[31:0] * rt[31:0] (accumulate and return 64-bit)
; This is more efficient than MULT + MADD + PMFHL.LW approach.
;===----------------------------------------------------------------------===;

; sum = (a*b) + (c*d) with 64-bit result
; Uses PMULTW + PMADDW chain - result directly in GPR, no extraction needed
; CHECK-LABEL: test_madd64_chain:
; CHECK: pmultw
; CHECK: pmaddw
; CHECK-NOT: pmfhl
; CHECK-NOT: daddu
define i64 @test_madd64_chain(i32 %a, i32 %b, i32 %c, i32 %d) {
  %a_ext = sext i32 %a to i64
  %b_ext = sext i32 %b to i64
  %c_ext = sext i32 %c to i64
  %d_ext = sext i32 %d to i64
  %mul1 = mul i64 %a_ext, %b_ext
  %mul2 = mul i64 %c_ext, %d_ext
  %result = add i64 %mul1, %mul2
  ret i64 %result
}

; Unsigned 64-bit accumulator chain
; CHECK-LABEL: test_maddu64_chain:
; CHECK: pmultuw
; CHECK: pmadduw
; CHECK-NOT: pmfhl
; CHECK-NOT: daddu
define i64 @test_maddu64_chain(i32 %a, i32 %b, i32 %c, i32 %d) {
  %a_ext = zext i32 %a to i64
  %b_ext = zext i32 %b to i64
  %c_ext = zext i32 %c to i64
  %d_ext = zext i32 %d to i64
  %mul1 = mul i64 %a_ext, %b_ext
  %mul2 = mul i64 %c_ext, %d_ext
  %result = add i64 %mul1, %mul2
  ret i64 %result
}

; sum = (a*b) + (c*d) + (e*f) - triple chain with 64-bit result
; Uses PMULTW + PMADDW + PMADDW chain
; CHECK-LABEL: test_madd64_triple:
; CHECK: pmultw
; CHECK: pmaddw
; CHECK: pmaddw
; CHECK-NOT: pmfhl
define i64 @test_madd64_triple(i32 %a, i32 %b, i32 %c, i32 %d, i32 %e, i32 %f) {
  %a_ext = sext i32 %a to i64
  %b_ext = sext i32 %b to i64
  %c_ext = sext i32 %c to i64
  %d_ext = sext i32 %d to i64
  %e_ext = sext i32 %e to i64
  %f_ext = sext i32 %f to i64
  %mul1 = mul i64 %a_ext, %b_ext
  %mul2 = mul i64 %c_ext, %d_ext
  %mul3 = mul i64 %e_ext, %f_ext
  %add1 = add i64 %mul1, %mul2
  %result = add i64 %add1, %mul3
  ret i64 %result
}

;===----------------------------------------------------------------------===;
; 64-bit MADD with Initial Accumulator
;===----------------------------------------------------------------------===;

; Widening multiply-add to 64-bit accumulator
; acc + (a*b) - single widening multiply with external accumulator
; Not optimized to PMULTW chain since acc is not from another multiply
; CHECK-LABEL: test_madd_with_acc:
; CHECK: mult
; CHECK: daddu
define i64 @test_madd_with_acc(i32 %a, i32 %b, i64 %acc) {
  %a_ext = sext i32 %a to i64
  %b_ext = sext i32 %b to i64
  %mul = mul i64 %a_ext, %b_ext
  %result = add i64 %mul, %acc
  ret i64 %result
}

;===----------------------------------------------------------------------===;
; 64-bit Multiply-Subtract Chain Tests (PMSUBW)
; PMSUBW: rd = HI:LO - rs[31:0] * rt[31:0]
; Used for patterns like (a*b) - (c*d) with 64-bit result
;===----------------------------------------------------------------------===;

; Simple subtract: (a*b) - (c*d) with 64-bit result
; Uses PMULTW + PMSUBW chain
; CHECK-LABEL: test_msub64_chain:
; CHECK: pmultw
; CHECK: pmsubw
; CHECK-NOT: pmfhl
; CHECK-NOT: dsubu
define i64 @test_msub64_chain(i32 %a, i32 %b, i32 %c, i32 %d) {
  %a_ext = sext i32 %a to i64
  %b_ext = sext i32 %b to i64
  %c_ext = sext i32 %c to i64
  %d_ext = sext i32 %d to i64
  %mul1 = mul i64 %a_ext, %b_ext
  %mul2 = mul i64 %c_ext, %d_ext
  %result = sub i64 %mul1, %mul2
  ret i64 %result
}

; Mixed add/sub: (a*b) + (c*d) - (e*f) with 64-bit result
; Uses PMULTW + PMADDW + PMSUBW chain
; CHECK-LABEL: test_madd_msub64_mixed:
; CHECK: pmultw
; CHECK: pmaddw
; CHECK: pmsubw
; CHECK-NOT: pmfhl
define i64 @test_madd_msub64_mixed(i32 %a, i32 %b, i32 %c, i32 %d, i32 %e, i32 %f) {
  %a_ext = sext i32 %a to i64
  %b_ext = sext i32 %b to i64
  %c_ext = sext i32 %c to i64
  %d_ext = sext i32 %d to i64
  %e_ext = sext i32 %e to i64
  %f_ext = sext i32 %f to i64
  %mul1 = mul i64 %a_ext, %b_ext
  %mul2 = mul i64 %c_ext, %d_ext
  %mul3 = mul i64 %e_ext, %f_ext
  %add1 = add i64 %mul1, %mul2
  %result = sub i64 %add1, %mul3
  ret i64 %result
}

; Cross product style: (a*b) - (c*d) useful for determinants
; CHECK-LABEL: test_cross_product:
; CHECK: pmultw
; CHECK: pmsubw
define i64 @test_cross_product(i32 %x1, i32 %y2, i32 %x2, i32 %y1) {
  %x1_ext = sext i32 %x1 to i64
  %y2_ext = sext i32 %y2 to i64
  %x2_ext = sext i32 %x2 to i64
  %y1_ext = sext i32 %y1 to i64
  %mul1 = mul i64 %x1_ext, %y2_ext
  %mul2 = mul i64 %x2_ext, %y1_ext
  %result = sub i64 %mul1, %mul2
  ret i64 %result
}
