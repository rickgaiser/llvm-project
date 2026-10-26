; RUN: llc -mtriple=vu -pass-remarks=pipeliner -pass-remarks-analysis=pipeliner \
; RUN:   -o /dev/null < %s 2>&1 | FileCheck %s
; RUN: llc -mtriple=vu -pass-remarks=pipeliner -pass-remarks-analysis=pipeliner \
; RUN:   -pass-remarks-missed=pipeliner -pipeliner-reduce-lifetimes=false \
; RUN:   -o /dev/null < %s 2>&1 | FileCheck %s --check-prefix=NOREDUCE
; RUN: llc -mtriple=vu -verify-machineinstrs -o /dev/null < %s

; The inner loop of maxsdk's fbm.hlsl (fractal noise, compiled for VU0): a
; long chain of FMAC instructions per point. Its iterative modulo schedules
; place each instruction as early as it fits, which keeps more values live
; than the 31 VF registers hold at every II of the search range. Lowering the
; registers over the limit first, before the words of the kernel, when the
; values are shortened finds a schedule that fits.

; CHECK: Minimal Initiation Interval: 76 (ResMII 76, RecMII 2)
; CHECK-NEXT: Schedule found with Initiation Interval: 103, MaxStageCount: 1
; CHECK-NEXT: Pipelined succesfully!

; NOREDUCE: Minimal Initiation Interval: 76 (ResMII 76, RecMII 2)
; NOREDUCE-NEXT: Unable to find schedule

target datalayout = "e-m:e-p:16:16-i32:32-i64:64-f32:32-v128:128-n16"
target triple = "vu"

define void @fbm() #0 {
vu.entry:
  %n = tail call i16 @llvm.vu.ilw(i16 0, i32 0)
  %0 = load i32, ptr addrspace(12) addrspacecast (ptr addrspace(1) inttoptr (i16 16 to ptr addrspace(1)) to ptr addrspace(12)), align 4
  %1 = zext i16 %n to i32
  %cmp.i3 = icmp eq i32 %0, 0
  %exitcond.not1 = icmp eq i16 %n, 0
  %or.cond = select i1 %cmp.i3, i1 true, i1 %exitcond.not1
  br i1 %or.cond, label %_Z3fbmv.exit, label %for.cond2.i.preheader

for.cond2.i.preheader:                            ; preds = %vu.entry, %for.cond2.i.for.cond.cleanup4.i_crit_edge
  %amp.0.i5 = phi float [ %mul55.i, %for.cond2.i.for.cond.cleanup4.i_crit_edge ], [ 5.000000e-01, %vu.entry ]
  %o.0.i4 = phi i32 [ %inc57.i, %for.cond2.i.for.cond.cleanup4.i_crit_edge ], [ 0, %vu.entry ]
  %2 = insertelement <4 x float> <float 2.000000e+00, float 2.000000e+00, float poison, float poison>, float %amp.0.i5, i64 2
  %3 = shufflevector <4 x float> %2, <4 x float> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 2>
  br label %for.body5.i

for.cond2.i.for.cond.cleanup4.i_crit_edge:        ; preds = %for.body5.i
  %mul55.i = fmul nnan ninf nsz arcp afn float %amp.0.i5, 5.000000e-01
  %inc57.i = add nuw i32 %o.0.i4, 1
  %cmp.i = icmp ult i32 %inc57.i, %0
  br i1 %cmp.i, label %for.cond2.i.preheader, label %_Z3fbmv.exit

for.body5.i:                                      ; preds = %for.cond2.i.preheader, %for.body5.i
  %i.0.i2 = phi i32 [ 0, %for.cond2.i.preheader ], [ %inc.i, %for.body5.i ]
  %4 = trunc i32 %i.0.i2 to i16
  %5 = getelementptr [16 x i8], ptr addrspace(4) inttoptr (i16 32 to ptr addrspace(4)), i16 %4
  %6 = addrspacecast ptr addrspace(4) %5 to ptr addrspace(11)
  %7 = load <4 x float>, ptr addrspace(11) %6, align 4
  %8 = extractelement <4 x float> %7, i64 0
  %9 = extractelement <4 x float> %7, i64 1
  %10 = tail call nnan ninf nsz arcp afn float @llvm.floor.f32(float %8)
  %11 = tail call nnan ninf nsz arcp afn float @llvm.floor.f32(float %9)
  %sub.i = fsub nnan ninf nsz arcp afn float %8, %10
  %sub6.i = fsub nnan ninf nsz arcp afn float %9, %11
  %splat.splatinsert.i = insertelement <4 x float> poison, float %10, i64 0
  %splat.splat.i = shufflevector <4 x float> %splat.splatinsert.i, <4 x float> poison, <4 x i32> zeroinitializer
  %add.i = fadd nnan ninf nsz arcp afn <4 x float> %splat.splat.i, <float 0.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>
  %splat.splatinsert7.i = insertelement <4 x float> poison, float %11, i64 0
  %splat.splat8.i = shufflevector <4 x float> %splat.splatinsert7.i, <4 x float> poison, <4 x i32> zeroinitializer
  %add9.i = fadd nnan ninf nsz arcp afn <4 x float> %splat.splat8.i, <float 0.000000e+00, float 0.000000e+00, float 1.000000e+00, float 1.000000e+00>
  %mul.i = fmul nnan ninf nsz arcp afn <4 x float> %add.i, splat (float 1.031000e-01)
  %12 = tail call nnan ninf nsz arcp afn <4 x float> @llvm.floor.v4f32(<4 x float> %mul.i)
  %13 = fsub nnan ninf nsz arcp afn <4 x float> %mul.i, %12
  %mul10.i = fmul nnan ninf nsz arcp afn <4 x float> %add9.i, splat (float 1.031000e-01)
  %14 = tail call nnan ninf nsz arcp afn <4 x float> @llvm.floor.v4f32(<4 x float> %mul10.i)
  %15 = fsub nnan ninf nsz arcp afn <4 x float> %mul10.i, %14
  %add12.i = fadd nnan ninf nsz arcp afn <4 x float> %15, splat (float 3.333000e+01)
  %mul13.i = fmul nnan ninf nsz arcp afn <4 x float> %13, %add12.i
  %add14.i = fadd nnan ninf nsz arcp afn <4 x float> %13, splat (float 3.333000e+01)
  %mul15.i = fmul nnan ninf nsz arcp afn <4 x float> %15, %add14.i
  %add16.i = fadd nnan ninf nsz arcp afn <4 x float> %mul15.i, %mul13.i
  %mul18.i = fmul nnan ninf nsz arcp afn <4 x float> %13, %add14.i
  %add19.i = fadd nnan ninf nsz arcp afn <4 x float> %mul18.i, %add16.i
  %add20.i = fadd nnan ninf nsz arcp afn <4 x float> %13, %add19.i
  %add21.i = fadd nnan ninf nsz arcp afn <4 x float> %15, %add20.i
  %add22.i = fadd nnan ninf nsz arcp afn <4 x float> %add19.i, %add21.i
  %mul24.i = fmul nnan ninf nsz arcp afn <4 x float> %add20.i, %add22.i
  %16 = tail call nnan ninf nsz arcp afn <4 x float> @llvm.floor.v4f32(<4 x float> %mul24.i)
  %17 = fsub nnan ninf nsz arcp afn <4 x float> %mul24.i, %16
  %mul26.i = fmul nnan ninf nsz arcp afn float %sub.i, %sub.i
  %mul27.i = fmul nnan ninf nsz arcp afn float %sub.i, 2.000000e+00
  %sub28.i = fsub nnan ninf nsz arcp afn float 3.000000e+00, %mul27.i
  %mul29.i = fmul nnan ninf nsz arcp afn float %mul26.i, %sub28.i
  %mul30.i = fmul nnan ninf nsz arcp afn float %sub6.i, %sub6.i
  %mul31.i = fmul nnan ninf nsz arcp afn float %sub6.i, 2.000000e+00
  %sub32.i = fsub nnan ninf nsz arcp afn float 3.000000e+00, %mul31.i
  %mul33.i = fmul nnan ninf nsz arcp afn float %mul30.i, %sub32.i
  %18 = shufflevector <4 x float> %17, <4 x float> poison, <2 x i32> <i32 3, i32 1>
  %19 = shufflevector <4 x float> %17, <4 x float> poison, <2 x i32> <i32 2, i32 0>
  %20 = fsub reassoc nnan ninf nsz arcp afn <2 x float> %18, %19
  %21 = insertelement <2 x float> poison, float %mul29.i, i64 0
  %22 = shufflevector <2 x float> %21, <2 x float> poison, <2 x i32> zeroinitializer
  %23 = fmul reassoc nnan ninf nsz arcp afn <2 x float> %20, %22
  %24 = fadd reassoc nnan ninf nsz arcp afn <2 x float> %23, %19
  %25 = extractelement <2 x float> %24, i64 0
  %26 = extractelement <2 x float> %24, i64 1
  %sub.i9 = fsub reassoc nnan ninf nsz arcp afn float %25, %26
  %mul.i10 = fmul reassoc nnan ninf nsz arcp afn float %sub.i9, %mul33.i
  %add.i11 = fadd reassoc nnan ninf nsz arcp afn float %mul.i10, %26
  %27 = shufflevector <4 x float> %7, <4 x float> <float poison, float poison, float 1.000000e+00, float 1.000000e+00>, <4 x i32> <i32 1, i32 0, i32 6, i32 7>
  %28 = fmul nnan ninf nsz arcp afn <4 x float> %27, <float 6.000000e-01, float 6.000000e-01, float -1.000000e+00, float -1.000000e+00>
  %29 = insertelement <4 x float> %7, float %add.i11, i64 2
  %30 = fmul nnan ninf nsz arcp afn <4 x float> %29, <float 8.000000e-01, float 8.000000e-01, float 2.000000e+00, float poison>
  %31 = shufflevector <4 x float> %30, <4 x float> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 2>
  %32 = fsub nnan ninf nsz arcp afn <4 x float> %31, %28
  %33 = fadd nnan ninf nsz arcp afn <4 x float> %31, %28
  %34 = shufflevector <4 x float> %32, <4 x float> %33, <4 x i32> <i32 0, i32 5, i32 6, i32 7>
  %35 = fmul nnan ninf nsz arcp afn <4 x float> %3, %34
  %36 = shufflevector <4 x float> %7, <4 x float> <float 1.710000e+01, float 3.700000e+00, float poison, float poison>, <4 x i32> <i32 4, i32 5, i32 2, i32 2>
  %37 = fadd nnan ninf nsz arcp afn <4 x float> %36, %35
  store <4 x float> %37, ptr addrspace(11) %6, align 4
  %inc.i = add nuw nsw i32 %i.0.i2, 1
  %exitcond.not = icmp eq i32 %inc.i, %1
  br i1 %exitcond.not, label %for.cond2.i.for.cond.cleanup4.i_crit_edge, label %for.body5.i

_Z3fbmv.exit:                                     ; preds = %for.cond2.i.for.cond.cleanup4.i_crit_edge, %vu.entry
  ret void
}

declare float @llvm.floor.f32(float)

declare <4 x float> @llvm.floor.v4f32(<4 x float>)

declare i16 @llvm.vu.ilw(i16, i32 immarg)

attributes #0 = { "target-cpu"="vu0" }
