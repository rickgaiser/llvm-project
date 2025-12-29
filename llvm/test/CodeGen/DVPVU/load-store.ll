; RUN: llc -mtriple=dvpvu < %s | FileCheck %s

; Test vector load (LQ)
; CHECK-LABEL: test_load_vector:
; CHECK: lq.xyzw
define <4 x float> @test_load_vector(ptr %p) {
  %v = load <4 x float>, ptr %p
  ret <4 x float> %v
}

; Test vector store (SQ)
; CHECK-LABEL: test_store_vector:
; CHECK: sq.xyzw
define void @test_store_vector(<4 x float> %v, ptr %p) {
  store <4 x float> %v, ptr %p
  ret void
}

; Test integer load (ILW)
; CHECK-LABEL: test_load_int:
; CHECK: ilw.x
define i16 @test_load_int(ptr %p) {
  %v = load i16, ptr %p
  ret i16 %v
}

; Test integer store (ISW)
; CHECK-LABEL: test_store_int:
; CHECK: isw.x
define void @test_store_int(i16 %v, ptr %p) {
  store i16 %v, ptr %p
  ret void
}
