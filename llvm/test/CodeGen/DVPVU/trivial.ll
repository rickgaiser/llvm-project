; RUN: llc -mtriple=dvpvu < %s | FileCheck %s
; Test trivial function that returns void

; CHECK-LABEL: empty_function:
define void @empty_function() {
  ret void
}
