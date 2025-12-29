; RUN: llc -mtriple=dvpvu < %s | FileCheck %s
; Test basic integer operations on VU i16 type

; XFAIL: *
; TODO: Add instruction selection patterns for integer ops

; CHECK-LABEL: test_iadd:
; CHECK: iadd vi{{[0-9]+}}, vi{{[0-9]+}}, vi{{[0-9]+}}
define i16 @test_iadd(i16 %a, i16 %b) {
  %result = add i16 %a, %b
  ret i16 %result
}

; CHECK-LABEL: test_isub:
; CHECK: isub vi{{[0-9]+}}, vi{{[0-9]+}}, vi{{[0-9]+}}
define i16 @test_isub(i16 %a, i16 %b) {
  %result = sub i16 %a, %b
  ret i16 %result
}

; CHECK-LABEL: test_iand:
; CHECK: iand vi{{[0-9]+}}, vi{{[0-9]+}}, vi{{[0-9]+}}
define i16 @test_iand(i16 %a, i16 %b) {
  %result = and i16 %a, %b
  ret i16 %result
}

; CHECK-LABEL: test_ior:
; CHECK: ior vi{{[0-9]+}}, vi{{[0-9]+}}, vi{{[0-9]+}}
define i16 @test_ior(i16 %a, i16 %b) {
  %result = or i16 %a, %b
  ret i16 %result
}
