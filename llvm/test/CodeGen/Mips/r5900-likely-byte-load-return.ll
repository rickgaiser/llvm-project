; REQUIRES: asserts
; RUN: llc -mtriple=mips64el -mcpu=r5900 -target-abi=n32 -O3 -debug-only=mips-subtarget -o /dev/null %s 2>&1 | FileCheck %s --check-prefix=R5900
; RUN: llc -mtriple=mips64el -mcpu=mips4 -target-abi=n32 -O3 -debug-only=mips-subtarget -o /dev/null %s 2>&1 | FileCheck %s --check-prefix=MIPS4 --allow-empty
;
; Preserve an explicitly likely byte-load path that falls through to a
; shared return on R5900, allowing the return delay slot to hold the load.
; Do not apply this when the path is unprofiled, or when returning a word.
; R5900: R5900 branch folder: preserve hot byte-load return in likely_byte
; R5900-NOT: R5900 branch folder: preserve hot byte-load return in unlikely_byte
; R5900-NOT: R5900 branch folder: preserve hot byte-load return in likely_word
; MIPS4-NOT: R5900 branch folder:

declare i8 @cold_byte(i32)
declare i32 @cold_word(i32)

define zeroext i8 @likely_byte(ptr %base, i32 %offset) {
entry:
  %cond = icmp eq i32 %offset, 123
  br i1 %cond, label %hot, label %cold, !prof !0
hot:
  %p = getelementptr i8, ptr %base, i32 %offset
  %v = load i8, ptr %p, align 1
  br label %done
cold:
  %w = call i8 @cold_byte(i32 %offset)
  br label %done
done:
  %result = phi i8 [ %v, %hot ], [ %w, %cold ]
  ret i8 %result
}

define zeroext i8 @unlikely_byte(ptr %base, i32 %offset) {
entry:
  %cond = icmp eq i32 %offset, 123
  br i1 %cond, label %hot, label %cold
hot:
  %p = getelementptr i8, ptr %base, i32 %offset
  %v = load i8, ptr %p, align 1
  br label %done
cold:
  %w = call i8 @cold_byte(i32 %offset)
  br label %done
done:
  %result = phi i8 [ %v, %hot ], [ %w, %cold ]
  ret i8 %result
}

define i32 @likely_word(ptr %base, i32 %offset) {
entry:
  %cond = icmp eq i32 %offset, 123
  br i1 %cond, label %hot, label %cold, !prof !0
hot:
  %p = getelementptr i32, ptr %base, i32 %offset
  %v = load i32, ptr %p, align 1
  br label %done
cold:
  %w = call i32 @cold_word(i32 %offset)
  br label %done
done:
  %result = phi i32 [ %v, %hot ], [ %w, %cold ]
  ret i32 %result
}

!0 = !{!"branch_weights", i32 2000, i32 1}
