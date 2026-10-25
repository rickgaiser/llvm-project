; RUN: split-file %s %t
; RUN: not llc -mtriple=vu %t/args.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=ARGS
; RUN: not llc -mtriple=vu %t/ret.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=RET
; RUN: not llc -mtriple=vu %t/load.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=LOAD
; RUN: not llc -mtriple=vu %t/call.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=CALL
; RUN: not llc -mtriple=vu -O3 %t/call-loop.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=CALL
; RUN: not llc -mtriple=vu -mcpu=vu0 %t/vu1.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=VU1
; RUN: not llc -mtriple=vu %t/mul.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=MUL
; RUN: not llc -mtriple=vu %t/shift.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=SHIFT
; RUN: not llc -mtriple=vu %t/div.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=DIV
; RUN: not llc -mtriple=vu %t/half.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=HALF
; RUN: not llc -mtriple=vu %t/room.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=ROOM
; RUN: not llc -mtriple=vu %t/indexed-carry.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=ICARRY
; RUN: not llc -mtriple=vu %t/indexed-bytes.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=IBYTES
; RUN: not llc -mtriple=vu %t/indexed-buffer.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=IBUF
; RUN: not llc -mtriple=vu %t/contexts.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=CTX
; RUN: not llc -mtriple=vu %t/contexts-room.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=CTXROOM

; ARGS: error: {{.*}} VU: micro programs take no arguments
; RET:  error: {{.*}} VU: micro programs do not return a value
; LOAD: error: {{.*}} VU: unsupported load type
; LOAD: error: {{.*}} VU: memory access that is not by quadwords
; CALL: error: {{.*}} VU: function calls are not supported
; VU1:  error: {{.*}} VU: XGKICK and XTOP require VU1
; MUL:  error: {{.*}} VU: integer multiply by a variable
; SHIFT: error: {{.*}} VU: shift by a variable
; DIV:  error: {{.*}} VU: integer division by a variable
; HALF: error: {{.*}} VU: 16-bit load from the high half of a field
; ROOM: error: {{.*}} VU: the buffer of 3 quadwords has no room for a batch
; ICARRY: error: {{.*}} VU: an indexed vertex program cannot carry prev from vertex to vertex
; IBUF: error: {{.*}} VU: an indexed vertex program sizes its buffers itself
; CTX: error: {{.*}} VU: a vertex program has one or two contexts
; CTXROOM: error: {{.*}} VU: two buffers of 400 quadwords, 2 contexts of 201 and 0 for spills do not fit VU1 data memory
; IBYTES: error: {{.*}} VU: 130 vertices of 2 outputs are too many for byte indices

;--- args.ll
define void @f(i16 %a) {
  ret void
}

;--- ret.ll
define i16 @f() {
  ret i16 0
}

;--- load.ll
; Bytes, and an index by floats: the VU addresses quadwords.
define void @f(i16 %i) {
  %v = load i8, ptr inttoptr (i16 16 to ptr)
  %p = getelementptr float, ptr inttoptr (i16 32 to ptr), i16 %i
  %f = load float, ptr %p
  ret void
}

;--- call.ll
declare void @g()
define void @f() {
  call void @g()
  ret void
}

;--- call-loop.ll
; A call in a loop: the error, and llc ends (it used to hang).
declare <4 x float> @g(<4 x float>)
declare <4 x float> @llvm.vu.lq(i16)
declare void @llvm.vu.sq(<4 x float>, i16)
define void @f() {
entry:
  br label %loop
loop:
  %i = phi i16 [ 0, %entry ], [ %n, %loop ]
  %v = call <4 x float> @llvm.vu.lq(i16 %i)
  %r = call <4 x float> @g(<4 x float> %v)
  call void @llvm.vu.sq(<4 x float> %r, i16 %i)
  %n = add i16 %i, 1
  %c = icmp eq i16 %n, 10
  br i1 %c, label %done, label %loop
done:
  ret void
}

;--- vu1.ll
declare void @llvm.vu.xgkick(i16)
define void @f() {
  call void @llvm.vu.xgkick(i16 0)
  ret void
}

;--- mul.ll
declare i16 @llvm.vu.ilw(i16, i32)
declare void @llvm.vu.isw(i16, i16, i32)
define void @f() {
  %a = call i16 @llvm.vu.ilw(i16 0, i32 0)
  %b = call i16 @llvm.vu.ilw(i16 1, i32 0)
  %m = mul i16 %a, %b
  call void @llvm.vu.isw(i16 %m, i16 2, i32 0)
  ret void
}

;--- shift.ll
declare i16 @llvm.vu.ilw(i16, i32)
declare void @llvm.vu.isw(i16, i16, i32)
define void @f() {
  %a = call i16 @llvm.vu.ilw(i16 0, i32 0)
  %b = call i16 @llvm.vu.ilw(i16 1, i32 0)
  %m = lshr i16 %a, %b
  call void @llvm.vu.isw(i16 %m, i16 2, i32 0)
  ret void
}

;--- div.ll
declare i16 @llvm.vu.ilw(i16, i32)
declare void @llvm.vu.isw(i16, i16, i32)
define void @f() {
  %a = call i16 @llvm.vu.ilw(i16 0, i32 0)
  %b = call i16 @llvm.vu.ilw(i16 1, i32 0)
  %m = udiv i16 %a, %b
  call void @llvm.vu.isw(i16 %m, i16 2, i32 0)
  ret void
}

;--- half.ll
; Element 1 of an int4 seen as <8 x i16> is the high half of field x.
define void @f() {
  %v = load <8 x i16>, ptr inttoptr (i16 16 to ptr)
  %a = extractelement <8 x i16> %v, i32 1
  store i16 %a, ptr inttoptr (i16 32 to ptr)
  ret void
}

;--- indexed-carry.ll
; A value carried from vertex to vertex: the vertices of an indexed program
; are not drawn in the order they are transformed.
@pos = external addrspace(2) global <4 x float>, !vu.input !0
@xyz2 = external addrspace(3) global <4 x float>, !vu.output !1
@prev = internal addrspace(5) global <4 x float> zeroinitializer, !vu.carry !2
define void @f() "vu-entry"="vertex" "vu-indexed"="16" {
  %p = load <4 x float>, ptr addrspace(2) @pos
  %q = load <4 x float>, ptr addrspace(5) @prev
  store <4 x float> %p, ptr addrspace(5) @prev
  %s = fadd <4 x float> %p, %q
  store <4 x float> %s, ptr addrspace(3) @xyz2
  ret void
}
!0 = !{i32 0, !"v4_32"}
!1 = !{i32 0, !"xyz2"}
!2 = !{}

;--- indexed-bytes.ll
; An index is a byte: 130 vertices of 2 quadwords do not fit (and the table
; leaves the buffers too small, too).
@pos = external addrspace(2) global <4 x float>, !vu.input !0
@rgbaq = external addrspace(3) global <4 x float>, !vu.output !1
@xyz2 = external addrspace(3) global <4 x float>, !vu.output !2
define void @f() "vu-entry"="vertex" "vu-indexed"="130" {
  %p = load <4 x float>, ptr addrspace(2) @pos
  store <4 x float> %p, ptr addrspace(3) @rgbaq
  store <4 x float> %p, ptr addrspace(3) @xyz2
  ret void
}
!0 = !{i32 0, !"v4_32"}
!1 = !{i32 0, !"rgbaq"}
!2 = !{i32 1, !"xyz2"}

;--- room.ll
; A buffer too small for a batch: the error, and no crash in the passes after
; the entry lowering.
@pos = external addrspace(2) global <4 x float>, !vu.input !0
@xyz2 = external addrspace(3) global <4 x float>, !vu.output !1
define void @f() "vu-entry"="vertex" "vu-buffer-qw"="3" {
  %p = load <4 x float>, ptr addrspace(2) @pos
  store <4 x float> %p, ptr addrspace(3) @xyz2
  ret void
}
!0 = !{i32 0, !"v4_32"}
!1 = !{i32 0, !"xyz2"}

;--- indexed-buffer.ll
; The layout of an indexed program sets the size of its buffers.
@pos = external addrspace(2) global <4 x float>, !vu.input !0
@xyz2 = external addrspace(3) global <4 x float>, !vu.output !1
define void @f() "vu-entry"="vertex" "vu-indexed"="16" "vu-buffer-qw"="256" {
  %p = load <4 x float>, ptr addrspace(2) @pos
  store <4 x float> %p, ptr addrspace(3) @xyz2
  ret void
}
!0 = !{i32 0, !"v4_32"}
!1 = !{i32 0, !"xyz2"}

;--- contexts.ll
@pos = external addrspace(2) global <4 x float>, !vu.input !0
@xyz2 = external addrspace(3) global <4 x float>, !vu.output !1
define void @f() "vu-entry"="vertex" "vu-contexts"="3" {
  %p = load <4 x float>, ptr addrspace(2) @pos
  store <4 x float> %p, ptr addrspace(3) @xyz2
  ret void
}
!0 = !{i32 0, !"v4_32"}
!1 = !{i32 0, !"xyz2"}

;--- contexts-room.ll
; The contexts take their room from the buffers.
@m = external addrspace(1) global [200 x <4 x float>], !vu.uniform !2
@pos = external addrspace(2) global <4 x float>, !vu.input !0
@xyz2 = external addrspace(3) global <4 x float>, !vu.output !1
define void @f() "vu-entry"="vertex" "vu-buffer-qw"="400" {
  %q = getelementptr <4 x float>, ptr addrspace(1) @m, i16 199
  %m = load <4 x float>, ptr addrspace(1) %q
  %p = load <4 x float>, ptr addrspace(2) @pos
  %r = fadd <4 x float> %p, %m
  store <4 x float> %r, ptr addrspace(3) @xyz2
  ret void
}
!0 = !{i32 0, !"v4_32"}
!1 = !{i32 0, !"xyz2"}
!2 = !{i32 0}
