; REQUIRES: asserts
; RUN: llc -mtriple=vu -mcpu=vu1 -O3 -debug-only=vu-instr-info,vu-loop-scheduler \
; RUN:   -o /dev/null < %s 2>&1 | FileCheck %s

; The vertex loop of maxsdk's mscei.hlsl (VU1). At II 21 three values live
; longer than an iteration. Modulo variable expansion would unroll the kernel
; five times, for the output pointer that the stores of the last stage read,
; not twice as the copies need; VULoopCopies turns that pointer into offsets.
; The copies leave more than a quarter of the lower slots free, so they are
; kept and the kernel is not unrolled: one loop of 21 words.

; CHECK:      VU: II 21 needs 21 upper and 14 lower words with 3 copies
; CHECK-NEXT: %bb.7: 21 words take 21 cycles
; CHECK-NEXT: %bb.7: 21 cycles, at least 21 words

target datalayout = "e-m:e-p:16:16-i32:32-i64:64-f32:32-v128:128-n16"
target triple = "vu"

@llvm.global_ctors = appending addrspace(10) global [0 x { i32, ptr, ptr }] zeroinitializer
@vu.header = private constant [1475 x i8] c"/* vertex program mscei: VU data memory layout, in quadwords */\0A#define VU_MSCEI_MVP 0\0A#define VU_MSCEI_A 4\0A#define VU_MSCEI_E 7\0A#define VU_MSCEI_AMB 10\0A#define VU_MSCEI_PRM 11\0A#define VU_MSCEI_SCL 12\0A#define VU_MSCEI_ENTRY 0  /* micro address, 64-bit units; MSCAL (ITOP = the context) for the first batch of a draw, MSCNT for the others */\0A#define VU_MSCEI_BUF_QW 498  /* double buffer size */\0A#define VU_MSCEI_UNI_QW 13  /* uniforms, after the context's GIF tag */\0A#define VU_MSCEI_CTX_AT 996  /* the contexts: GIF tag (NLOOP 0), uniforms */\0A#define VU_MSCEI_CTX_QW 14  /* per context */\0A#define VU_MSCEI_NCTX 2  /* contexts (1: a FLUSHE before writing it) */\0A#define VU_MSCEI_ITEM_QW 3  /* quadwords per vertex in */\0A#define VU_MSCEI_OUT_QW 3  /* GIF registers per vertex */\0A#define VU_MSCEI_MAX_N 81  /* vertices per batch */\0A#define VU_MSCEI_ALIGN 4  /* batches start at multiples of it */\0A#define VU_MSCEI_COUNTS 0  /* x vertices, z index quadwords, w indices */\0A#define VU_MSCEI_IN_AT 1  /* first item */\0A#define VU_MSCEI_OUT_AT 253  /* the output GIF tag */\0A#define VU_MSCEI_NREG 3\0A#define VU_MSCEI_REGS 0x512ull  /* GIF register list */\0A#define VU_MSCEI_NIN 3\0A#define VU_MSCEI_IN_POSITION 0  /* v3_32, 12 bytes */\0A#define VU_MSCEI_IN_NORMAL 1  /* v4_8, 4 bytes */\0A#define VU_MSCEI_IN_TEXCOORD 2  /* v2_16, 4 bytes */\0A#define VU_MSCEI_INPUTS {{0x8, 0, 12, 0x0}, {0xe, 0, 4, 0x0}, {0x5, 0, 4, 0x0}}  /* UNPACK format, unsigned, bytes, STMASK (fields 1.0 from COL) */\0A\0A", section ".vu.header", align 1
@llvm.used = appending global [1 x ptr] [ptr @vu.header], section "llvm.metadata"

; Function Attrs: mustprogress nofree noinline norecurse noreturn nosync nounwind willreturn memory(readwrite, argmem: none, inaccessiblemem: none, target_mem: none)
define void @mscei() local_unnamed_addr #0 {
vu.entry:
  %ctx = tail call i16 @llvm.vu.xitop()
  %uni = shl i16 %ctx, 4
  %0 = add i16 %uni, 16
  %1 = inttoptr i16 %0 to ptr addrspace(1)
  %2 = addrspacecast ptr addrspace(1) %1 to ptr addrspace(12)
  %3 = load <16 x float>, ptr addrspace(12) %2, align 4, !tbaa !6, !alias.scope !8, !noalias !11
  %4 = add i16 %uni, 80
  %5 = inttoptr i16 %4 to ptr addrspace(1)
  %6 = addrspacecast ptr addrspace(1) %5 to ptr addrspace(12)
  %7 = load <3 x float>, ptr addrspace(12) %6, align 4, !alias.scope !8, !noalias !11
  %8 = getelementptr inbounds nuw i8, ptr addrspace(12) %6, i16 16
  %9 = load <3 x float>, ptr addrspace(12) %8, align 4, !alias.scope !8, !noalias !11
  %10 = getelementptr inbounds nuw i8, ptr addrspace(12) %6, i16 32
  %11 = load <3 x float>, ptr addrspace(12) %10, align 4, !alias.scope !8, !noalias !11
  %12 = add i16 %uni, 128
  %13 = inttoptr i16 %12 to ptr addrspace(1)
  %14 = addrspacecast ptr addrspace(1) %13 to ptr addrspace(12)
  %15 = load <3 x float>, ptr addrspace(12) %14, align 4, !alias.scope !8, !noalias !11
  %16 = getelementptr inbounds nuw i8, ptr addrspace(12) %14, i16 16
  %17 = load <3 x float>, ptr addrspace(12) %16, align 4, !alias.scope !8, !noalias !11
  %18 = getelementptr inbounds nuw i8, ptr addrspace(12) %14, i16 32
  %19 = load <3 x float>, ptr addrspace(12) %18, align 4, !alias.scope !8, !noalias !11
  %20 = add i16 %uni, 176
  %21 = inttoptr i16 %20 to ptr addrspace(1)
  %22 = addrspacecast ptr addrspace(1) %21 to ptr addrspace(12)
  %23 = load <4 x float>, ptr addrspace(12) %22, align 4, !alias.scope !8, !noalias !11
  %24 = add i16 %uni, 208
  %25 = inttoptr i16 %24 to ptr addrspace(1)
  %26 = addrspacecast ptr addrspace(1) %25 to ptr addrspace(12)
  %27 = load <3 x float>, ptr addrspace(12) %26, align 4, !alias.scope !8, !noalias !11
  %28 = add i16 %uni, 192
  %29 = inttoptr i16 %28 to ptr addrspace(1)
  %30 = addrspacecast ptr addrspace(1) %29 to ptr addrspace(12)
  %31 = load <3 x float>, ptr addrspace(12) %30, align 4, !alias.scope !8, !noalias !11
  %32 = shufflevector <16 x float> %3, <16 x float> poison, <4 x i32> <i32 12, i32 13, i32 14, i32 15>
  %33 = shufflevector <16 x float> %3, <16 x float> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 3>
  %34 = shufflevector <16 x float> %3, <16 x float> poison, <4 x i32> <i32 4, i32 5, i32 6, i32 7>
  %35 = shufflevector <16 x float> %3, <16 x float> poison, <4 x i32> <i32 8, i32 9, i32 10, i32 11>
  %36 = shufflevector <4 x float> %23, <4 x float> poison, <3 x i32> <i32 0, i32 1, i32 2>
  br label %vu.batch

vu.batch:                                         ; preds = %vu.done, %vu.entry
  %top = tail call i16 @llvm.vu.xtop()
  %n = tail call i16 @llvm.vu.ilw(i16 %top, i32 0)
  %ctx7 = tail call i16 @llvm.vu.xitop()
  %tag = tail call <4 x float> @llvm.vu.lq(i16 %ctx7)
  %eop = tail call i16 @llvm.vu.ilw(i16 %ctx7, i32 0)
  %tag.x = or i16 %eop, %n
  %out = add i16 %top, 253
  %empty = icmp eq i16 %n, 0
  tail call void @llvm.vu.sq(<4 x float> %tag, i16 %out)
  tail call void @llvm.vu.isw(i16 %tag.x, i16 %out, i32 0)
  br i1 %empty, label %vu.done, label %vu.item.preheader

vu.item.preheader:                                ; preds = %vu.batch
  %o1 = add i16 %top, 254
  %in = add i16 %top, 1
  br label %vu.item

vu.item:                                          ; preds = %vu.item.preheader, %vu.item
  %i = phi i16 [ %i.next, %vu.item ], [ 0, %vu.item.preheader ]
  %pi = phi i16 [ %74, %vu.item ], [ %in, %vu.item.preheader ]
  %po = phi i16 [ %75, %vu.item ], [ %o1, %vu.item.preheader ]
  %37 = shl i16 %pi, 4
  %38 = inttoptr i16 %37 to ptr addrspace(2)
  %39 = add i16 %37, 16
  %40 = inttoptr i16 %39 to ptr addrspace(2)
  %41 = add i16 %37, 32
  %42 = inttoptr i16 %41 to ptr addrspace(2)
  %43 = shl i16 %po, 4
  %44 = inttoptr i16 %43 to ptr addrspace(3)
  %45 = add i16 %43, 16
  %46 = inttoptr i16 %45 to ptr addrspace(3)
  %47 = add i16 %43, 32
  %48 = inttoptr i16 %47 to ptr addrspace(3)
  %49 = addrspacecast ptr addrspace(2) %38 to ptr addrspace(7)
  %50 = load <3 x float>, ptr addrspace(7) %49, align 4, !alias.scope !8, !noalias !15
  %51 = addrspacecast ptr addrspace(2) %40 to ptr addrspace(7)
  %52 = load <3 x i32>, ptr addrspace(7) %51, align 4, !alias.scope !8, !noalias !15
  %53 = addrspacecast ptr addrspace(2) %42 to ptr addrspace(7)
  %54 = load <2 x i32>, ptr addrspace(7) %53, align 4, !alias.scope !8, !noalias !15
  %.splat2 = shufflevector <3 x float> %50, <3 x float> poison, <4 x i32> zeroinitializer
  %55 = fmul reassoc nnan ninf nsz arcp afn <4 x float> %.splat2, %33
  %56 = fadd nnan ninf nsz arcp afn <4 x float> %32, %55
  %.splat4 = shufflevector <3 x float> %50, <3 x float> poison, <4 x i32> <i32 1, i32 1, i32 1, i32 1>
  %57 = fmul reassoc nnan ninf nsz arcp afn <4 x float> %.splat4, %34
  %58 = fadd nnan ninf nsz arcp afn <4 x float> %57, %56
  %.splat6 = shufflevector <3 x float> %50, <3 x float> poison, <4 x i32> <i32 2, i32 2, i32 2, i32 2>
  %59 = fmul reassoc nnan ninf nsz arcp afn <4 x float> %.splat6, %35
  %60 = fadd nnan ninf nsz arcp afn <4 x float> %59, %58
  %61 = extractelement <4 x float> %60, i64 3
  %hlsl.rcp.i = fdiv reassoc nnan ninf nsz arcp afn float 1.000000e+00, %61
  %62 = shufflevector <4 x float> %60, <4 x float> poison, <3 x i32> <i32 0, i32 1, i32 2>
  %splat.splatinsert.i = insertelement <3 x float> poison, float %hlsl.rcp.i, i64 0
  %splat.splat.i = shufflevector <3 x float> %splat.splatinsert.i, <3 x float> poison, <3 x i32> zeroinitializer
  %mul.i = fmul reassoc nnan ninf nsz arcp afn <3 x float> %62, %splat.splat.i
  %conv.i = sitofp reassoc nnan ninf nsz arcp afn <3 x i32> %52 to <3 x float>
  %splat.splat9.i = shufflevector <3 x float> %conv.i, <3 x float> poison, <3 x i32> zeroinitializer
  %mul10.i = fmul reassoc nnan ninf nsz arcp afn <3 x float> %splat.splat9.i, %7
  %splat.splat12.i = shufflevector <3 x float> %conv.i, <3 x float> poison, <3 x i32> <i32 1, i32 1, i32 1>
  %mul13.i = fmul reassoc nnan ninf nsz arcp afn <3 x float> %splat.splat12.i, %9
  %add.i = fadd reassoc nnan ninf nsz arcp afn <3 x float> %mul13.i, %mul10.i
  %splat.splat15.i = shufflevector <3 x float> %conv.i, <3 x float> poison, <3 x i32> <i32 2, i32 2, i32 2>
  %mul16.i = fmul reassoc nnan ninf nsz arcp afn <3 x float> %splat.splat15.i, %11
  %add17.i = fadd reassoc nnan ninf nsz arcp afn <3 x float> %add.i, %mul16.i
  %elt.max.i = tail call reassoc nnan ninf nsz arcp afn <3 x float> @llvm.maxnum.v3f32(<3 x float> %add17.i, <3 x float> zeroinitializer)
  %splat.splat19.i = shufflevector <3 x float> %elt.max.i, <3 x float> poison, <3 x i32> zeroinitializer
  %mul20.i = fmul reassoc nnan ninf nsz arcp afn <3 x float> %splat.splat19.i, %15
  %splat.splat22.i = shufflevector <3 x float> %elt.max.i, <3 x float> poison, <3 x i32> <i32 1, i32 1, i32 1>
  %mul23.i = fmul reassoc nnan ninf nsz arcp afn <3 x float> %splat.splat22.i, %17
  %splat.splat26.i = shufflevector <3 x float> %elt.max.i, <3 x float> poison, <3 x i32> <i32 2, i32 2, i32 2>
  %mul27.i = fmul reassoc nnan ninf nsz arcp afn <3 x float> %splat.splat26.i, %19
  %add24.i = fadd reassoc nnan ninf nsz arcp afn <3 x float> %mul23.i, %36
  %add28.i = fadd reassoc nnan ninf nsz arcp afn <3 x float> %add24.i, %mul20.i
  %add29.i = fadd reassoc nnan ninf nsz arcp afn <3 x float> %add28.i, %mul27.i
  %.scalar = fmul reassoc nnan ninf nsz arcp afn float %hlsl.rcp.i, f0x39800000
  %63 = insertelement <2 x float> poison, float %.scalar, i64 0
  %64 = shufflevector <2 x float> %63, <2 x float> poison, <4 x i32> <i32 0, i32 0, i32 poison, i32 poison>
  %65 = shufflevector <2 x i32> %54, <2 x i32> poison, <4 x i32> <i32 0, i32 1, i32 poison, i32 poison>
  %66 = sitofp reassoc nnan ninf nsz arcp afn <4 x i32> %65 to <4 x float>
  %67 = fmul reassoc nnan ninf nsz arcp afn <4 x float> %64, %66
  %vecinit39.i = insertelement <4 x float> %67, float %hlsl.rcp.i, i64 2
  %68 = tail call reassoc nnan ninf nsz arcp afn <3 x float> @llvm.minnum.v3f32(<3 x float> %add29.i, <3 x float> splat (float 2.550000e+02))
  %vecinit45.i = shufflevector <3 x float> %68, <3 x float> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 poison>
  %vecinit46.i = shufflevector <4 x float> %vecinit45.i, <4 x float> %23, <4 x i32> <i32 0, i32 1, i32 2, i32 7>
  %add.i.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %vecinit46.i, splat (float f0x4B400000)
  %mul49.i = fmul nnan ninf nsz arcp afn <3 x float> %27, %mul.i
  %add50.i = fadd nnan ninf nsz arcp afn <3 x float> %31, %mul49.i
  %69 = shufflevector <3 x float> %add50.i, <3 x float> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 poison>
  %vecinit6377.i = insertelement <4 x float> %69, float 0.000000e+00, i64 3
  %mul.i.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %vecinit6377.i, splat (float 1.600000e+01)
  %conv.i.i = fptosi <4 x float> %mul.i.i to <4 x i32>
  %70 = addrspacecast ptr addrspace(3) %44 to ptr addrspace(8)
  %71 = shufflevector <4 x float> %vecinit39.i, <4 x float> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 poison>
  store <4 x float> %71, ptr addrspace(8) %70, align 4, !alias.scope !15, !noalias !8
  %72 = addrspacecast ptr addrspace(3) %46 to ptr addrspace(8)
  store <4 x float> %add.i.i, ptr addrspace(8) %72, align 4, !alias.scope !15, !noalias !8
  %73 = addrspacecast ptr addrspace(3) %48 to ptr addrspace(8)
  store <4 x i32> %conv.i.i, ptr addrspace(8) %73, align 4, !alias.scope !15, !noalias !8
  %i.next = add nuw i16 %i, 1
  %74 = add i16 %pi, 3
  %75 = add i16 %po, 3
  %last = icmp eq i16 %i.next, %n
  br i1 %last, label %vu.done, label %vu.item

vu.done:                                          ; preds = %vu.item, %vu.batch
  tail call void @llvm.vu.xgkick(i16 %out)
  tail call void @llvm.vu.yield()
  br label %vu.batch
}

; Function Attrs: mustprogress nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare <3 x float> @llvm.maxnum.v3f32(<3 x float>, <3 x float>) #1

; Function Attrs: mustprogress nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare <3 x float> @llvm.minnum.v3f32(<3 x float>, <3 x float>) #1

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(inaccessiblemem: readwrite)
declare i16 @llvm.vu.xitop() #2

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(inaccessiblemem: readwrite)
declare i16 @llvm.vu.xtop() #2

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(read)
declare i16 @llvm.vu.ilw(i16, i32 immarg) #3

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(read)
declare <4 x float> @llvm.vu.lq(i16) #3

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn
declare void @llvm.vu.xgkick(i16) #4

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn
declare void @llvm.vu.yield() #4

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(write)
declare void @llvm.vu.sq(<4 x float>, i16) #5

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(write)
declare void @llvm.vu.isw(i16, i16, i32 immarg) #5

attributes #0 = { mustprogress nofree noinline norecurse noreturn nosync nounwind willreturn memory(readwrite, argmem: none, inaccessiblemem: none, target_mem: none) "no-signed-zeros-fp-math"="false" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="vu1" "vu-header-prefix"="VU_" }
attributes #1 = { mustprogress nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none) }
attributes #2 = { mustprogress nocallback nofree nosync nounwind willreturn memory(inaccessiblemem: readwrite) }
attributes #3 = { mustprogress nocallback nofree nosync nounwind willreturn memory(read) }
attributes #4 = { mustprogress nocallback nofree nosync nounwind willreturn }
attributes #5 = { mustprogress nocallback nofree nosync nounwind willreturn memory(write) }

!llvm.ident = !{!0}
!llvm.errno.tbaa = !{!1}

!0 = !{!"clang version 24.0.0git (https://github.com/rickgaiser/llvm-project af4807c366f1789bdecc6e612521c110c4a8dd98)"}
!1 = !{!2, !3, i64 0}
!2 = !{!"__libc_errno", !3, i64 0}
!3 = !{!"int", !4, i64 0}
!4 = !{!"omnipotent char", !5, i64 0}
!5 = !{!"Simple C++ TBAA"}
!6 = !{!7, !7, i64 0}
!7 = !{!"float", !4, i64 0}
!8 = !{!9}
!9 = distinct !{!9, !10, !"in"}
!10 = distinct !{!10, i1 false, !"mscei"}
!11 = !{!12, !14}
!12 = distinct !{!12, !13, !"_Z5mscei2In: %agg.result"}
!13 = distinct !{!13, i1 true, !"_Z5mscei2In"}
!14 = distinct !{!14, !10, !"out"}
!15 = !{!14}
