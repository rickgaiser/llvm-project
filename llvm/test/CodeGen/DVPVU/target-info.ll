; RUN: llc -mtriple=dvpvu --mcpu=help 2>&1 | FileCheck %s

; Test that the DVPVU target is properly registered and provides expected info

; CHECK: Available CPUs for this target:
; CHECK: vu0
; CHECK: vu1

; CHECK: Available features for this target:
; CHECK: efu
; CHECK: vu1
