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

;===----------------------------------------------------------------------===;
; Widening Multiplication Tests (32x32 = 64-bit result)
;===----------------------------------------------------------------------===;

; Signed widening multiply - needs both HI and LO
; CHECK-LABEL: test_smul_wide:
; CHECK: mult
; CHECK-NOT: dmult
; CHECK-DAG: mflo
; CHECK-DAG: mfhi
define i64 @test_smul_wide(i32 signext %a, i32 signext %b) {
  %a_ext = sext i32 %a to i64
  %b_ext = sext i32 %b to i64
  %result = mul i64 %a_ext, %b_ext
  ret i64 %result
}

; Unsigned widening multiply
; CHECK-LABEL: test_umul_wide:
; CHECK: multu
; CHECK-NOT: dmultu
; CHECK-DAG: mflo
; CHECK-DAG: mfhi
define i64 @test_umul_wide(i32 zeroext %a, i32 zeroext %b) {
  %a_ext = zext i32 %a to i64
  %b_ext = zext i32 %b to i64
  %result = mul i64 %a_ext, %b_ext
  ret i64 %result
}

;===----------------------------------------------------------------------===;
; MULHS/MULHU Tests (get high 32 bits of 64-bit product)
;===----------------------------------------------------------------------===;

; MULHS - get high 32 bits of signed multiply
; CHECK-LABEL: test_mulhs:
; CHECK: mult
; CHECK: mfhi
define i32 @test_mulhs(i32 signext %a, i32 signext %b) {
  %a_ext = sext i32 %a to i64
  %b_ext = sext i32 %b to i64
  %mul = mul i64 %a_ext, %b_ext
  %hi = lshr i64 %mul, 32
  %result = trunc i64 %hi to i32
  ret i32 %result
}

; MULHU - get high 32 bits of unsigned multiply
; CHECK-LABEL: test_mulhu:
; CHECK: multu
; CHECK: mfhi
define i32 @test_mulhu(i32 zeroext %a, i32 zeroext %b) {
  %a_ext = zext i32 %a to i64
  %b_ext = zext i32 %b to i64
  %mul = mul i64 %a_ext, %b_ext
  %hi = lshr i64 %mul, 32
  %result = trunc i64 %hi to i32
  ret i32 %result
}

;===----------------------------------------------------------------------===;
; Negative Tests - DMULT/DMULTU must NOT be generated for R5900
;===----------------------------------------------------------------------===;

; 64-bit multiply must NOT use DMULT on R5900
; R5900 doesn't support DMULT - must expand to 32-bit operations
; CHECK-LABEL: test_mul64:
; CHECK-NOT: {{^[[:space:]]+dmult[[:space:]]}}
; CHECK-NOT: {{^[[:space:]]+dmultu[[:space:]]}}
define i64 @test_mul64(i64 %a, i64 %b) {
  %result = mul i64 %a, %b
  ret i64 %result
}

;===----------------------------------------------------------------------===;
; Small Integer Promotion Tests (i8/i16 -> i32)
;===----------------------------------------------------------------------===;

; i8 multiplication promoted to i32
; CHECK-LABEL: test_mult_i8:
; CHECK: mult
define i8 @test_mult_i8(i8 signext %a, i8 signext %b) {
  %result = mul i8 %a, %b
  ret i8 %result
}

; i16 multiplication promoted to i32
; CHECK-LABEL: test_mult_i16:
; CHECK: mult
define i16 @test_mult_i16(i16 signext %a, i16 signext %b) {
  %result = mul i16 %a, %b
  ret i16 %result
}

; i8 widening to i16
; CHECK-LABEL: test_mult_i8_wide:
; CHECK: mult
define i16 @test_mult_i8_wide(i8 signext %a, i8 signext %b) {
  %a_ext = sext i8 %a to i16
  %b_ext = sext i8 %b to i16
  %result = mul i16 %a_ext, %b_ext
  ret i16 %result
}
