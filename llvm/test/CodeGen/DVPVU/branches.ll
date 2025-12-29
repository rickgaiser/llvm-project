; RUN: llc -mtriple=dvpvu < %s | FileCheck %s
; Test branch and control flow operations

; XFAIL: *
; TODO: Add branch lowering patterns

; VU has 1-instruction delay slot after branches
; The branch instructions are:
; - B offset       (unconditional branch)
; - BAL rd, offset (branch and link)
; - JR rs          (jump register)
; - JALR rd, rs    (jump and link register)
; - IBEQ rs, rt, offset (branch if equal)
; - IBNE rs, rt, offset (branch if not equal)
; - IBGTZ rs, offset    (branch if greater than zero)
; - IBGEZ rs, offset    (branch if greater or equal zero)
; - IBLTZ rs, offset    (branch if less than zero)
; - IBLEZ rs, offset    (branch if less or equal zero)

; CHECK-LABEL: test_loop:
; CHECK: iaddi
; CHECK: ibne
define i16 @test_loop(i16 %n) {
entry:
  br label %loop

loop:
  %i = phi i16 [ 0, %entry ], [ %i.next, %loop ]
  %i.next = add i16 %i, 1
  %cond = icmp eq i16 %i.next, %n
  br i1 %cond, label %exit, label %loop

exit:
  ret i16 %i.next
}

; CHECK-LABEL: test_conditional:
; CHECK: ibgtz
define i16 @test_conditional(i16 %a, i16 %b) {
  %cmp = icmp sgt i16 %a, 0
  br i1 %cmp, label %then, label %else

then:
  ret i16 %a

else:
  ret i16 %b
}
