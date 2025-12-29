; RUN: llc -mtriple=dvpvu < %s | FileCheck %s
; Test load/store operations

; XFAIL: *
; TODO: Add load/store patterns

; VU memory operations:
; - LQ.dest vt, offset(is)    (load quadword - 128-bit)
; - SQ.dest vt, offset(is)    (store quadword - 128-bit)
; - LQI.dest vt, (is++)       (load with post-increment)
; - SQI.dest vt, (is++)       (store with post-increment)
; - ILW.f it, offset(is)      (load word - 32-bit)
; - ISW.f it, offset(is)      (store word - 32-bit)

; CHECK-LABEL: test_load_vector:
; CHECK: lq.xyzw
define <4 x float> @test_load_vector(ptr %p) {
  %val = load <4 x float>, ptr %p
  ret <4 x float> %val
}

; CHECK-LABEL: test_store_vector:
; CHECK: sq.xyzw
define void @test_store_vector(ptr %p, <4 x float> %val) {
  store <4 x float> %val, ptr %p
  ret void
}
