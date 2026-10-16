; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a -O2 < %s | FileCheck %s

; The R3000A issues one instruction per cycle. The post-RA scheduler must count
; cycles that way, or it spends independent instructions too early and leaves
; the jump-table load right before the jump, which then needs a load-delay nop.

; CHECK-LABEL: k_state:
; CHECK:         lw $1, %lo($JTI0_0)($1)
; CHECK-NEXT:    lbu $5, 0($3)
; CHECK-NEXT:    jr $1
; CHECK:         lw $1, %lo($JTI0_0)($1)
; CHECK-NEXT:    lbu $5, 0($3)
; CHECK-NEXT:    jr $1

@buf = global [1024 x i8] zeroinitializer
define i32 @k_state() {
entry:
  br label %for.body
for.cond.cleanup:                                 ; preds = %sw.epilog
  ret i32 %add28
for.body:                                         ; preds = %entry, %sw.epilog
  %i.047 = phi i32 [ 0, %entry ], [ %inc, %sw.epilog ]
  %s.046 = phi i32 [ 0, %entry ], [ %add28, %sw.epilog ]
  %st.045 = phi i32 [ 0, %entry ], [ %st.1, %sw.epilog ]
  %arrayidx = getelementptr inbounds nuw i8, ptr @buf, i32 %i.047
  %0 = load i8, ptr %arrayidx
  %conv = zext i8 %0 to i32
  switch i32 %st.045, label %default.unreachable48 [
    i32 0, label %sw.bb
    i32 1, label %sw.bb5
    i32 2, label %sw.bb11
    i32 3, label %sw.bb17
  ]
sw.bb:                                            ; preds = %for.body
  %and = and i32 %conv, 1
  %tobool.not = icmp eq i32 %and, 0
  %and2 = and i32 %conv, 2
  %spec.select = select i1 %tobool.not, i32 %and2, i32 1
  br label %sw.epilog
sw.bb5:                                           ; preds = %for.body
  %cmp7 = icmp ugt i8 %0, -128
  %cond9 = select i1 %cmp7, i32 3, i32 0
  %add = add i32 %s.046, %conv
  br label %sw.epilog
sw.bb11:                                          ; preds = %for.body
  %and13 = and i32 %conv, 4
  %tobool14.not = icmp eq i32 %and13, 0
  %cond15 = select i1 %tobool14.not, i32 3, i32 0
  %shl = shl nuw nsw i32 %conv, 1
  %xor = xor i32 %shl, %s.046
  br label %sw.epilog
sw.bb17:                                          ; preds = %for.body
  %shr = lshr i32 %conv, 2
  %shl20 = shl nuw nsw i32 %conv, 6
  %or = add i32 %shl20, %s.046
  %add21 = add i32 %or, %shr
  br label %sw.epilog
default.unreachable48:                            ; preds = %for.body
  unreachable
sw.epilog:                                        ; preds = %sw.bb17, %sw.bb11, %sw.bb5, %sw.bb
  %st.1 = phi i32 [ 0, %sw.bb17 ], [ %cond15, %sw.bb11 ], [ %cond9, %sw.bb5 ], [ %spec.select, %sw.bb ]
  %s.1 = phi i32 [ %add21, %sw.bb17 ], [ %xor, %sw.bb11 ], [ %add, %sw.bb5 ], [ %s.046, %sw.bb ]
  %and23 = and i32 %conv, 1
  %shr25 = lshr i32 %conv, 1
  %and26 = and i32 %shr25, 1
  %add27 = add nuw nsw i32 %and26, %and23
  %add28 = add i32 %add27, %s.1
  %inc = add nuw nsw i32 %i.047, 1
  %exitcond.not = icmp eq i32 %inc, 1024
  br i1 %exitcond.not, label %for.cond.cleanup, label %for.body
}
