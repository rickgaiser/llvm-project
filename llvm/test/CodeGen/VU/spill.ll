; RUN: llc -mtriple=vu -mcpu=vu1 -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -mtriple=vu -mcpu=vu0 -verify-machineinstrs < %s | FileCheck %s --check-prefix=VU0
; RUN: sed 's/"vu-spill-qw"="8"/"vu-spill-qw"="1"/' %s > %t.ll
; RUN: not llc -mtriple=vu -mcpu=vu1 < %t.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=LIMIT

; Spill slots are quadwords at the top of VU data memory: 1023 down on VU1,
; 255 down on VU0, addressed from VI0. "vu-spill-qw" limits their number.

; CHECK-LABEL: spill:
; CHECK: sq.xyzw vf{{[0-9]+}}, 1023(vi0)
; CHECK: lq.xyzw vf{{[0-9]+}}, 1023(vi0)
; VU0-LABEL: spill:
; VU0: sq.xyzw vf{{[0-9]+}}, 255(vi0)
; LIMIT: error: {{.*}} VU: the spills need {{[0-9]+}} quadwords at the top of data memory, more than the 1 reserved ("vu-spill-qw")

declare <4 x float> @llvm.vu.lq(i16)
declare void @llvm.vu.sq(<4 x float>, i16)
declare i16 @llvm.vu.ilw(i16, i32)
declare void @llvm.vu.isw(i16, i16, i32)

; 34 values live at once, for 31 VF registers.
define void @spill() #0 {
  %a = call i16 @llvm.vu.ilw(i16 0, i32 0)
  %v0 = call <4 x float> @llvm.vu.lq(i16 1)
  %v1 = call <4 x float> @llvm.vu.lq(i16 2)
  %v2 = call <4 x float> @llvm.vu.lq(i16 3)
  %v3 = call <4 x float> @llvm.vu.lq(i16 4)
  %v4 = call <4 x float> @llvm.vu.lq(i16 5)
  %v5 = call <4 x float> @llvm.vu.lq(i16 6)
  %v6 = call <4 x float> @llvm.vu.lq(i16 7)
  %v7 = call <4 x float> @llvm.vu.lq(i16 8)
  %v8 = call <4 x float> @llvm.vu.lq(i16 9)
  %v9 = call <4 x float> @llvm.vu.lq(i16 10)
  %v10 = call <4 x float> @llvm.vu.lq(i16 11)
  %v11 = call <4 x float> @llvm.vu.lq(i16 12)
  %v12 = call <4 x float> @llvm.vu.lq(i16 13)
  %v13 = call <4 x float> @llvm.vu.lq(i16 14)
  %v14 = call <4 x float> @llvm.vu.lq(i16 15)
  %v15 = call <4 x float> @llvm.vu.lq(i16 16)
  %v16 = call <4 x float> @llvm.vu.lq(i16 17)
  %v17 = call <4 x float> @llvm.vu.lq(i16 18)
  %v18 = call <4 x float> @llvm.vu.lq(i16 19)
  %v19 = call <4 x float> @llvm.vu.lq(i16 20)
  %v20 = call <4 x float> @llvm.vu.lq(i16 21)
  %v21 = call <4 x float> @llvm.vu.lq(i16 22)
  %v22 = call <4 x float> @llvm.vu.lq(i16 23)
  %v23 = call <4 x float> @llvm.vu.lq(i16 24)
  %v24 = call <4 x float> @llvm.vu.lq(i16 25)
  %v25 = call <4 x float> @llvm.vu.lq(i16 26)
  %v26 = call <4 x float> @llvm.vu.lq(i16 27)
  %v27 = call <4 x float> @llvm.vu.lq(i16 28)
  %v28 = call <4 x float> @llvm.vu.lq(i16 29)
  %v29 = call <4 x float> @llvm.vu.lq(i16 30)
  %v30 = call <4 x float> @llvm.vu.lq(i16 31)
  %v31 = call <4 x float> @llvm.vu.lq(i16 32)
  %v32 = call <4 x float> @llvm.vu.lq(i16 33)
  %v33 = call <4 x float> @llvm.vu.lq(i16 34)
  ; a store to an unknown address: the loads stay above it
  call void @llvm.vu.sq(<4 x float> zeroinitializer, i16 %a)
  %i = call i16 @llvm.vu.ilw(i16 0, i32 1)
  %j = add i16 %i, %i
  %w = call <4 x float> @llvm.vu.lq(i16 0)
  %s0 = fadd <4 x float> %w, %v0
  %s1 = fadd <4 x float> %s0, %v1
  %s2 = fadd <4 x float> %s1, %v2
  %s3 = fadd <4 x float> %s2, %v3
  %s4 = fadd <4 x float> %s3, %v4
  %s5 = fadd <4 x float> %s4, %v5
  %s6 = fadd <4 x float> %s5, %v6
  %s7 = fadd <4 x float> %s6, %v7
  %s8 = fadd <4 x float> %s7, %v8
  %s9 = fadd <4 x float> %s8, %v9
  %s10 = fadd <4 x float> %s9, %v10
  %s11 = fadd <4 x float> %s10, %v11
  %s12 = fadd <4 x float> %s11, %v12
  %s13 = fadd <4 x float> %s12, %v13
  %s14 = fadd <4 x float> %s13, %v14
  %s15 = fadd <4 x float> %s14, %v15
  %s16 = fadd <4 x float> %s15, %v16
  %s17 = fadd <4 x float> %s16, %v17
  %s18 = fadd <4 x float> %s17, %v18
  %s19 = fadd <4 x float> %s18, %v19
  %s20 = fadd <4 x float> %s19, %v20
  %s21 = fadd <4 x float> %s20, %v21
  %s22 = fadd <4 x float> %s21, %v22
  %s23 = fadd <4 x float> %s22, %v23
  %s24 = fadd <4 x float> %s23, %v24
  %s25 = fadd <4 x float> %s24, %v25
  %s26 = fadd <4 x float> %s25, %v26
  %s27 = fadd <4 x float> %s26, %v27
  %s28 = fadd <4 x float> %s27, %v28
  %s29 = fadd <4 x float> %s28, %v29
  %s30 = fadd <4 x float> %s29, %v30
  %s31 = fadd <4 x float> %s30, %v31
  %s32 = fadd <4 x float> %s31, %v32
  %s33 = fadd <4 x float> %s32, %v33
  call void @llvm.vu.sq(<4 x float> %s33, i16 100)
  call void @llvm.vu.isw(i16 %j, i16 101, i32 0)
  ret void
}

attributes #0 = { "vu-spill-qw"="8" }
