; RUN: llc -mtriple=mips64el -mcpu=r5900 < %s | FileCheck %s -check-prefix=FIX
; RUN: llc -mtriple=mips64el -mcpu=r5900 -mattr=-fix-r5900 < %s | FileCheck %s -check-prefix=NOFIX
;
; Test R5900 short loop erratum fix.
; The R5900 has a hardware bug where short loops (6 instructions or fewer)
; with a branch may exit after 1-2 iterations instead of the expected count.
; The fix ensures the delay slot contains a NOP for such short backward branches.

; Short loop test with store - delay slot can be filled when fix is disabled
; With fix enabled: bnez followed by nop
; With fix disabled: bnez followed by daddiu (delay slot filled)

; FIX-LABEL: test_short_loop_store:
; FIX:       .LBB0_1:
; FIX:       bnez
; FIX-NEXT:  nop

; NOFIX-LABEL: test_short_loop_store:
; NOFIX:     .LBB0_1:
; NOFIX:     bnez
; NOFIX-NEXT: daddiu
define void @test_short_loop_store(ptr %arr, i32 %n) {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %ptr = getelementptr i32, ptr %arr, i32 %i
  store i32 %i, ptr %ptr
  %inc = add i32 %i, 1
  %cmp = icmp slt i32 %inc, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}

; Long loop - should NOT be affected by the fix (too many instructions)
; Delay slot can be filled since loop is longer than 6 instructions
; The scheduler may choose different instructions for the delay slot
; FIX-LABEL: test_long_loop:
; FIX:       .LBB1_1:
; FIX:       bnez ${{[0-9]+}}, .LBB1_1
; Delay slot should be filled with actual instruction, not nop
; FIX-NEXT:  {{sw|daddiu|addu}}

; NOFIX-LABEL: test_long_loop:
; NOFIX:     .LBB1_1:
; NOFIX:     bnez ${{[0-9]+}}, .LBB1_1
; NOFIX-NEXT: {{sw|daddiu|addu}}
define i32 @test_long_loop(ptr %arr, i32 %n) {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %sum = phi i32 [ 0, %entry ], [ %add, %loop ]
  %ptr = getelementptr i32, ptr %arr, i32 %i
  %val = load i32, ptr %ptr
  %mul = mul i32 %val, %i
  %add = add i32 %sum, %mul
  %ptr2 = getelementptr i32, ptr %arr, i32 %add
  store i32 %add, ptr %ptr2
  %inc = add i32 %i, 1
  %cmp = icmp slt i32 %inc, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret i32 %add
}

; An inline asm statement counts as the instructions it holds: four here, so
; the loop is long and its delay slot is filled.

; FIX-LABEL: test_asm_long_loop:
; FIX:       .LBB2_1:
; FIX:       sq $9, 48($4)
; FIX:       bnez
; FIX-NEXT:  daddiu $4, $4, 64

define void @test_asm_long_loop(ptr %p, i32 %n) {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %q = phi ptr [ %p, %entry ], [ %q.next, %loop ]
  call void asm sideeffect "lq $$8, 0($0)\0A\09lq $$9, 16($0)\0A\09sq $$8, 32($0)\0A\09sq $$9, 48($0)", "r,~{$8},~{$9},~{memory}"(ptr %q)
  %q.next = getelementptr i8, ptr %q, i32 64
  %i.next = add i32 %i, 1
  %c = icmp ne i32 %i.next, %n
  br i1 %c, label %loop, label %exit

exit:
  ret void
}

; Directives, labels and comments are not instructions: this loop is short.

; FIX-LABEL: test_asm_short_loop:
; FIX:       .LBB3_1:
; FIX:       bnez
; FIX-NEXT:  nop

define void @test_asm_short_loop(ptr %p, i32 %n) {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %q = phi ptr [ %p, %entry ], [ %q.next, %loop ]
  call void asm sideeffect ".set push\0A\09.set noat # a comment\0A1:\0A\09sw $$0, 0($0)\0A\09.set pop", "r,~{memory}"(ptr %q)
  %q.next = getelementptr i8, ptr %q, i32 64
  %i.next = add i32 %i, 1
  %c = icmp ne i32 %i.next, %n
  br i1 %c, label %loop, label %exit

exit:
  ret void
}
