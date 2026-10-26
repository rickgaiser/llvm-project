; REQUIRES: asserts
; RUN: llc -mtriple=vu -vu-acc-order=false -debug-only=vu-loop-scheduler \
; RUN:   -o /dev/null < %s 2>&1 \
; RUN:   | FileCheck %s

; The loop of maxsdk's lvl3.hlsl. Proving that the iteration does not fit
; into a number of words just below the shortest schedule can take many
; steps: each length searches with at most half of the steps that are left,
; so the longer lengths still get searched. (With the ACC sums in source
; order: the loop is one that is hard to prove.)

; CHECK:      %bb.3: 49 cycles, at least 42 words
; CHECK:        no schedule in 43 words
; CHECK-NEXT:   no schedule found in {{[0-9]+}} steps
; CHECK-NEXT:   no schedule in 44 words
; CHECK-NEXT:   schedule in 45 words found in {{[0-9]+}} steps
; CHECK-NEXT: %bb.3: 49 cycles, 45 words take 45

target datalayout = "e-m:e-p:16:16-i32:32-i64:64-f32:32-v128:128-n16"
target triple = "vu"

define void @lvl3() local_unnamed_addr #0 {
vu.entry:
  %top = tail call i16 @llvm.vu.xtop()
  %nloop = tail call i16 @llvm.vu.ilw(i16 %top, i32 0)
  %n = and i16 %nloop, 32767
  %tag = tail call <4 x float> @llvm.vu.lq(i16 %top)
  %out = add i16 %top, 301
  %uni = shl i16 %top, 4
  %0 = add i16 %uni, 16
  %1 = inttoptr i16 %0 to ptr addrspace(1)
  %2 = add i16 %uni, 80
  %3 = inttoptr i16 %2 to ptr addrspace(1)
  %4 = add i16 %uni, 128
  %5 = inttoptr i16 %4 to ptr addrspace(1)
  %6 = add i16 %uni, 192
  %7 = inttoptr i16 %6 to ptr addrspace(1)
  %empty = icmp eq i16 %n, 0
  %8 = addrspacecast ptr addrspace(1) %1 to ptr addrspace(12)
  %9 = load <16 x float>, ptr addrspace(12) %8, align 4, !tbaa !6, !alias.scope !8, !noalias !11
  %10 = addrspacecast ptr addrspace(1) %3 to ptr addrspace(12)
  %11 = load <4 x float>, ptr addrspace(12) %10, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %12 = getelementptr inbounds nuw i8, ptr addrspace(12) %10, i16 16
  %13 = load <4 x float>, ptr addrspace(12) %12, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %14 = getelementptr inbounds nuw i8, ptr addrspace(12) %10, i16 32
  %15 = load <4 x float>, ptr addrspace(12) %14, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %16 = addrspacecast ptr addrspace(1) %5 to ptr addrspace(12)
  %17 = getelementptr inbounds nuw i8, ptr addrspace(12) %16, i16 48
  %18 = load <4 x float>, ptr addrspace(12) %17, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %19 = load <4 x float>, ptr addrspace(12) %16, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %20 = getelementptr inbounds nuw i8, ptr addrspace(12) %16, i16 16
  %21 = load <4 x float>, ptr addrspace(12) %20, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %22 = getelementptr inbounds nuw i8, ptr addrspace(12) %16, i16 32
  %23 = load <4 x float>, ptr addrspace(12) %22, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %24 = addrspacecast ptr addrspace(1) %7 to ptr addrspace(12)
  %25 = load float, ptr addrspace(12) %24, align 4, !alias.scope !8, !noalias !11
  %26 = getelementptr inbounds nuw i8, ptr addrspace(12) %24, i16 4
  %27 = load float, ptr addrspace(12) %26, align 4, !alias.scope !8, !noalias !11
  %28 = getelementptr inbounds nuw i8, ptr addrspace(12) %24, i16 8
  %29 = load float, ptr addrspace(12) %28, align 4, !alias.scope !8, !noalias !11
  %30 = getelementptr inbounds nuw i8, ptr addrspace(12) %24, i16 12
  %31 = load float, ptr addrspace(12) %30, align 4, !alias.scope !8, !noalias !11
  tail call void @llvm.vu.sq(<4 x float> %tag, i16 %out)
  br i1 %empty, label %vu.done, label %vu.item.preheader

vu.item.preheader:                                ; preds = %vu.entry
  %o1 = add i16 %top, 302
  %in = add i16 %top, 13
  %32 = shufflevector <16 x float> %9, <16 x float> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 3>
  %33 = shufflevector <16 x float> %9, <16 x float> poison, <4 x i32> <i32 4, i32 5, i32 6, i32 7>
  %34 = shufflevector <16 x float> %9, <16 x float> poison, <4 x i32> <i32 8, i32 9, i32 10, i32 11>
  %35 = shufflevector <16 x float> %9, <16 x float> poison, <4 x i32> <i32 12, i32 13, i32 14, i32 15>
  br label %vu.item

vu.item:                                          ; preds = %vu.item.preheader, %vu.item
  %i = phi i16 [ %i.next, %vu.item ], [ 0, %vu.item.preheader ]
  %pi = phi i16 [ %82, %vu.item ], [ %in, %vu.item.preheader ]
  %po = phi i16 [ %83, %vu.item ], [ %o1, %vu.item.preheader ]
  %_ZL3exs.0 = phi <2 x float> [ %vecinit44.i, %vu.item ], [ zeroinitializer, %vu.item.preheader ]
  %36 = shl i16 %pi, 4
  %37 = inttoptr i16 %36 to ptr addrspace(2)
  %38 = add i16 %36, 16
  %39 = inttoptr i16 %38 to ptr addrspace(2)
  %40 = add i16 %36, 32
  %41 = inttoptr i16 %40 to ptr addrspace(2)
  %42 = add i16 %36, 48
  %43 = inttoptr i16 %42 to ptr addrspace(2)
  %44 = shl i16 %po, 4
  %45 = inttoptr i16 %44 to ptr addrspace(3)
  %46 = add i16 %44, 16
  %47 = inttoptr i16 %46 to ptr addrspace(3)
  %48 = add i16 %44, 32
  %49 = inttoptr i16 %48 to ptr addrspace(3)
  %50 = addrspacecast ptr addrspace(2) %37 to ptr addrspace(7)
  %51 = load <3 x float>, ptr addrspace(7) %50, align 4, !alias.scope !8, !noalias !16
  %52 = addrspacecast ptr addrspace(2) %39 to ptr addrspace(7)
  %53 = load <4 x i32>, ptr addrspace(7) %52, align 4, !alias.scope !8, !noalias !16
  %54 = addrspacecast ptr addrspace(2) %41 to ptr addrspace(7)
  %55 = load <4 x i32>, ptr addrspace(7) %54, align 4, !alias.scope !8, !noalias !16
  %56 = addrspacecast ptr addrspace(2) %43 to ptr addrspace(7)
  %57 = load <2 x i32>, ptr addrspace(7) %56, align 4, !alias.scope !8, !noalias !16
  %.splat = shufflevector <3 x float> %51, <3 x float> poison, <4 x i32> zeroinitializer
  %58 = fmul reassoc nnan ninf nsz arcp afn <4 x float> %.splat, %32
  %.splat2 = shufflevector <3 x float> %51, <3 x float> poison, <4 x i32> <i32 1, i32 1, i32 1, i32 1>
  %59 = fmul reassoc nnan ninf nsz arcp afn <4 x float> %.splat2, %33
  %.splat4 = shufflevector <3 x float> %51, <3 x float> poison, <4 x i32> <i32 2, i32 2, i32 2, i32 2>
  %60 = fmul reassoc nnan ninf nsz arcp afn <4 x float> %.splat4, %34
  %61 = fadd reassoc nnan ninf nsz arcp afn <4 x float> %59, %35
  %62 = fadd reassoc nnan ninf nsz arcp afn <4 x float> %61, %58
  %63 = fadd reassoc nnan ninf nsz arcp afn <4 x float> %62, %60
  %64 = extractelement <4 x float> %63, i64 3
  %hlsl.rcp.i = fdiv reassoc nnan ninf nsz arcp afn float 1.000000e+00, %64
  %conv.i = sitofp reassoc nnan ninf nsz arcp afn <4 x i32> %53 to <4 x float>
  %splat.splat.i = shufflevector <4 x float> %conv.i, <4 x float> poison, <4 x i32> <i32 0, i32 0, i32 0, i32 poison>
  %mul.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %splat.splat.i, %11
  %splat.splat9.i = shufflevector <4 x float> %conv.i, <4 x float> poison, <4 x i32> <i32 1, i32 1, i32 1, i32 poison>
  %mul10.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %splat.splat9.i, %13
  %add.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %mul10.i, %mul.i
  %splat.splat12.i = shufflevector <4 x float> %conv.i, <4 x float> poison, <4 x i32> <i32 2, i32 2, i32 2, i32 poison>
  %mul13.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %splat.splat12.i, %15
  %add14.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %add.i, %mul13.i
  %elt.max.i = tail call reassoc nnan ninf nsz arcp afn <4 x float> @llvm.maxnum.v4f32(<4 x float> %add14.i, <4 x float> <float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float poison>)
  %splat.splat16.i = shufflevector <4 x float> %elt.max.i, <4 x float> poison, <4 x i32> zeroinitializer
  %mul17.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %splat.splat16.i, %19
  %add18.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %mul17.i, %18
  %splat.splat20.i = shufflevector <4 x float> %elt.max.i, <4 x float> poison, <4 x i32> <i32 1, i32 1, i32 1, i32 1>
  %mul21.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %splat.splat20.i, %21
  %add22.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %add18.i, %mul21.i
  %splat.splat24.i = shufflevector <4 x float> %elt.max.i, <4 x float> poison, <4 x i32> <i32 2, i32 2, i32 2, i32 2>
  %mul25.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %splat.splat24.i, %23
  %add26.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %add22.i, %mul25.i
  %mul27.i = fmul reassoc nnan ninf nsz arcp afn float %64, %25
  %add28.i = fadd reassoc nnan ninf nsz arcp afn float %mul27.i, %27
  %65 = tail call reassoc nnan ninf nsz arcp afn float @llvm.maxnum.f32(float %add28.i, float 0.000000e+00)
  %66 = tail call reassoc nnan ninf nsz arcp afn float @llvm.minnum.f32(float %65, float 2.550000e+02)
  %mul29.i = fmul reassoc nnan ninf nsz arcp afn float %64, %29
  %splat.splatinsert30.i = insertelement <4 x float> poison, float %mul29.i, i64 0
  %splat.splat31.i = shufflevector <4 x float> %splat.splatinsert30.i, <4 x float> poison, <4 x i32> <i32 0, i32 0, i32 poison, i32 poison>
  %sub.i = fsub reassoc nnan ninf nsz arcp afn <4 x float> %63, %splat.splat31.i
  %elt.abs.i = tail call reassoc nnan ninf nsz arcp afn <4 x float> @llvm.fabs.v4f32(<4 x float> %sub.i)
  %mul32.i = fmul reassoc nnan ninf nsz arcp afn float %64, %31
  %splat.splatinsert33.i = insertelement <4 x float> poison, float %mul32.i, i64 0
  %splat.splat34.i = shufflevector <4 x float> %splat.splatinsert33.i, <4 x float> poison, <4 x i32> <i32 0, i32 0, i32 poison, i32 poison>
  %sub35.i = fsub reassoc nnan ninf nsz arcp afn <4 x float> %elt.abs.i, %splat.splat34.i
  %67 = extractelement <4 x float> %sub35.i, i64 0
  %68 = extractelement <4 x float> %sub35.i, i64 1
  %elt.max36.i = tail call reassoc nnan ninf nsz arcp afn float @llvm.maxnum.f32(float %67, float %68)
  %foldExtExtBinop = fsub reassoc nnan ninf nsz arcp afn <4 x float> %11, %63
  %sub37.i = extractelement <4 x float> %foldExtExtBinop, i64 3
  %elt.max38.i = tail call reassoc nnan ninf nsz arcp afn float @llvm.maxnum.f32(float %elt.max36.i, float %sub37.i)
  %69 = extractelement <2 x float> %_ZL3exs.0, i64 0
  %70 = extractelement <2 x float> %_ZL3exs.0, i64 1
  %elt.max39.i = tail call reassoc nnan ninf nsz arcp afn float @llvm.maxnum.f32(float %69, float %70)
  %elt.max40.i = tail call reassoc nnan ninf nsz arcp afn float @llvm.maxnum.f32(float %elt.max38.i, float %elt.max39.i)
  %mul41.i = fmul reassoc nnan ninf nsz arcp afn float %elt.max40.i, 1.000000e+09
  %71 = tail call reassoc nnan ninf nsz arcp afn float @llvm.maxnum.f32(float %mul41.i, float 0.000000e+00)
  %72 = tail call reassoc nnan ninf nsz arcp afn float @llvm.minnum.f32(float %71, float 2.048000e+03)
  %vecinit43.i = insertelement <2 x float> poison, float %elt.max38.i, i64 0
  %vecinit44.i = shufflevector <2 x float> %vecinit43.i, <2 x float> %_ZL3exs.0, <2 x i32> <i32 0, i32 2>
  %.scalar = fmul reassoc nnan ninf nsz arcp afn float %hlsl.rcp.i, f0x39800000
  %73 = insertelement <2 x float> poison, float %.scalar, i64 0
  %74 = shufflevector <2 x float> %73, <2 x float> poison, <4 x i32> <i32 0, i32 0, i32 poison, i32 poison>
  %75 = shufflevector <2 x i32> %57, <2 x i32> poison, <4 x i32> <i32 0, i32 1, i32 poison, i32 poison>
  %76 = sitofp reassoc nnan ninf nsz arcp afn <4 x i32> %75 to <4 x float>
  %77 = fmul reassoc nnan ninf nsz arcp afn <4 x float> %74, %76
  %78 = insertelement <4 x float> %77, float 0.000000e+00, i64 3
  %vecinit54.i = insertelement <4 x float> %78, float %hlsl.rcp.i, i64 2
  %conv55.i = sitofp reassoc nnan ninf nsz arcp afn <4 x i32> %55 to <4 x float>
  %mul56.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %add26.i, %conv55.i
  %elt.min.i = tail call reassoc nnan ninf nsz arcp afn <4 x float> @llvm.minnum.v4f32(<4 x float> %mul56.i, <4 x float> splat (float 2.550000e+02))
  %conv57.i = fptosi <4 x float> %elt.min.i to <4 x i32>
  %splat.splatinsert59.i = insertelement <4 x float> poison, float %hlsl.rcp.i, i64 0
  %splat.splat60.i = shufflevector <4 x float> %splat.splatinsert59.i, <4 x float> poison, <4 x i32> <i32 0, i32 0, i32 0, i32 poison>
  %mul61.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %splat.splat60.i, %63
  %add78.i = fadd reassoc nnan ninf nsz arcp afn float %72, %66
  %vecinit79.i = insertelement <4 x float> %mul61.i, float %add78.i, i64 3
  %mul.i.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %vecinit79.i, splat (float 1.600000e+01)
  %conv.i.i = fptosi <4 x float> %mul.i.i to <4 x i32>
  %79 = addrspacecast ptr addrspace(3) %45 to ptr addrspace(8)
  store <4 x float> %vecinit54.i, ptr addrspace(8) %79, align 4, !alias.scope !16, !noalias !8
  %80 = addrspacecast ptr addrspace(3) %47 to ptr addrspace(8)
  store <4 x i32> %conv57.i, ptr addrspace(8) %80, align 4, !alias.scope !16, !noalias !8
  %81 = addrspacecast ptr addrspace(3) %49 to ptr addrspace(8)
  store <4 x i32> %conv.i.i, ptr addrspace(8) %81, align 4, !alias.scope !16, !noalias !8
  %i.next = add nuw nsw i16 %i, 1
  %82 = add i16 %pi, 4
  %83 = add i16 %po, 3
  %last = icmp eq i16 %i.next, %n
  br i1 %last, label %vu.done, label %vu.item

vu.done:                                          ; preds = %vu.item, %vu.entry
  tail call void @llvm.vu.xgkick(i16 %out)
  ret void
}

declare <4 x float> @llvm.maxnum.v4f32(<4 x float>, <4 x float>) #1

declare <4 x float> @llvm.fabs.v4f32(<4 x float>) #1

declare float @llvm.maxnum.f32(float, float) #1

declare <4 x float> @llvm.minnum.v4f32(<4 x float>, <4 x float>) #1

declare float @llvm.minnum.f32(float, float) #1

declare i16 @llvm.vu.xtop() #2

declare i16 @llvm.vu.ilw(i16, i32 immarg) #3

declare <4 x float> @llvm.vu.lq(i16) #3

declare void @llvm.vu.xgkick(i16) #4

declare void @llvm.vu.sq(<4 x float>, i16) #5

attributes #0 = { mustprogress nofree noinline norecurse nosync nounwind willreturn memory(readwrite, argmem: none, inaccessiblemem: none, target_mem: none) "no-signed-zeros-fp-math"="false" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="vu1" "vu-header-prefix"="VU_" }
attributes #1 = { mustprogress nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none) }
attributes #2 = { mustprogress nocallback nofree nosync nounwind willreturn memory(inaccessiblemem: readwrite) }
attributes #3 = { mustprogress nocallback nofree nosync nounwind willreturn memory(read) }
attributes #4 = { mustprogress nocallback nofree nosync nounwind willreturn }
attributes #5 = { mustprogress nocallback nofree nosync nounwind willreturn memory(write) }

!llvm.errno.tbaa = !{!1}

!1 = !{!2, !3, i64 0}
!2 = !{!"__libc_errno", !3, i64 0}
!3 = !{!"int", !4, i64 0}
!4 = !{!"omnipotent char", !5, i64 0}
!5 = !{!"Simple C++ TBAA"}
!6 = !{!7, !7, i64 0}
!7 = !{!"float", !4, i64 0}
!8 = !{!9}
!9 = distinct !{!9, !10, !"in"}
!10 = distinct !{!10, i1 false, !"lvl3"}
!11 = !{!12, !14}
!12 = distinct !{!12, !13, !"_Z4lvl32In: %agg.result"}
!13 = distinct !{!13, i1 true, !"_Z4lvl32In"}
!14 = distinct !{!14, !10, !"out"}
!15 = !{!4, !4, i64 0}
!16 = !{!14}
