; RUN: llc -mtriple=mips64el -mcpu=mips3 -target-abi n32 -O3 < %s | FileCheck %s --check-prefix=CHECK
; RUN: llc -mtriple=mips64el -mcpu=r5900 -target-abi n32 -O3 < %s | FileCheck %s --check-prefix=CHECK
;
; A zeroext i8 return from a multi-path PHI should not mask an already
; zero-extended LBu on the hot path. Only the callback path needs the mask.
;
; CHECK-LABEL: read_byte:
; CHECK: # %bb.1:{{.*}}ramload
; CHECK: jr $ra
; CHECK-NEXT: lbu $2, 0($1)
; CHECK: # %bb.3:{{.*}}call
; CHECK: andi $2, {{.*}}, 255
;
@ram = external global ptr

declare zeroext i16 @callback(i32)

define zeroext i8 @read_byte(i32 %index) {
entry:
  %which = and i32 %index, 3
  %isram = icmp eq i32 %which, 1
  br i1 %isram, label %ramload, label %other, !prof !0

ramload:
  %base = load ptr, ptr @ram
  %p = getelementptr i8, ptr %base, i32 %index
  %byte = load i8, ptr %p
  br label %done

other:
  %iscall = icmp eq i32 %which, 2
  br i1 %iscall, label %call, label %done

call:
  %wide = call zeroext i16 @callback(i32 %index)
  %shift = lshr i16 %wide, 3
  %lo = trunc i16 %shift to i8
  br label %done

done:
  %result = phi i8 [ %byte, %ramload ], [ %lo, %call ], [ -1, %other ]
  ret i8 %result
}

; Do not eliminate an i8 return mask if neither path is known extended.
; CHECK-LABEL: unknown_byte:
; CHECK: andi $2, {{.*}}, 255
define zeroext i8 @unknown_byte(i32 %x, i32 %y, i1 %select) {
entry:
  br i1 %select, label %a, label %b
a:
  %av = trunc i32 %x to i8
  br label %done
b:
  %bv = trunc i32 %y to i8
  br label %done
done:
  %v = phi i8 [ %av, %a ], [ %bv, %b ]
  ret i8 %v
}

; An LHu is not a zero-extended byte; it needs a mask for an i8 return.
; CHECK-LABEL: mixed_load_width:
; CHECK: # %bb.1:{{.*}}bytepath
; CHECK: jr $ra
; CHECK-NEXT: lbu
; CHECK: .LBB2_2:{{.*}}halfpath
; CHECK: lhu
; CHECK: andi
define zeroext i8 @mixed_load_width(ptr %p, ptr %q, i1 %which) {
entry:
  br i1 %which, label %bytepath, label %halfpath
bytepath:
  %v8 = load i8, ptr %p
  br label %done
halfpath:
  %v16 = load volatile i16, ptr %q
  %low8 = trunc i16 %v16 to i8
  br label %done
done:
  %v = phi i8 [ %v8, %bytepath ], [ %low8, %halfpath ]
  ret i8 %v
}

!0 = !{!"branch_weights", i32 10000, i32 1}
