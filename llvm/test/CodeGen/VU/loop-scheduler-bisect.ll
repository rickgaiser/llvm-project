; REQUIRES: asserts
; RUN: llc -mtriple=vu -mcpu=vu1 -O3 -debug-only=vu-loop-scheduler \
; RUN:   -o /dev/null < %s 2>&1 \
; RUN:   | FileCheck %s

; The vertex loop of maxsdk's mskin.hlsl (skinning with a blend of four
; joints, VU1). After register allocation the kernel stalls: 89 words take 101
; cycles. The search from the shortest length runs out of steps far below
; that, and the lengths in between are bisected.

; CHECK:      %bb.4: 89 words take 101 cycles
; CHECK-NEXT: %bb.4: 101 cycles, at least 73 words
; CHECK:        no schedule in 77 words
; CHECK-NEXT:   schedule in 89 words found in {{[0-9]+}} steps
; CHECK-NEXT: %bb.4: 101 cycles, 89 words take 89
; CHECK:        no schedule in 87 words
; CHECK-NEXT:   schedule in 88 words found in {{[0-9]+}} steps
; CHECK-NEXT: %bb.4: 101 cycles, 88 words take 88
; CHECK:      %bb.4: 88 cycles, at least 73 words

target datalayout = "e-m:e-p:16:16-i32:32-i64:64-f32:32-v128:128-n16"
target triple = "vu"

@llvm.global_ctors = appending addrspace(10) global [0 x { i32, ptr, ptr }] zeroinitializer
@vu.header = private constant [1620 x i8] c"/* vertex program mskin: VU data memory layout, in quadwords */\0A#define VU_MSKIN_MVP 0\0A#define VU_MSKIN_LIGHT 4\0A#define VU_MSKIN_LCOL 5\0A#define VU_MSKIN_AMB 6\0A#define VU_MSKIN_EYE 7\0A#define VU_MSKIN_JOINT 8\0A#define VU_MSKIN_ENTRY 0  /* micro address, 64-bit units; MSCAL (ITOP = the context) for the first batch of a draw, MSCNT for the others */\0A#define VU_MSKIN_BUF_QW 407  /* double buffer size */\0A#define VU_MSKIN_UNI_QW 104  /* uniforms, after the context's GIF tag */\0A#define VU_MSKIN_CTX_AT 814  /* the contexts: GIF tag (NLOOP 0), uniforms */\0A#define VU_MSKIN_CTX_QW 105  /* per context */\0A#define VU_MSKIN_NCTX 2  /* contexts (1: a FLUSHE before writing it) */\0A#define VU_MSKIN_ITEM_QW 5  /* quadwords per vertex in */\0A#define VU_MSKIN_OUT_QW 3  /* GIF registers per vertex */\0A#define VU_MSKIN_MAX_N 46  /* vertices per batch */\0A#define VU_MSKIN_ALIGN 8  /* batches start at multiples of it */\0A#define VU_MSKIN_COUNTS 0  /* x vertices, z index quadwords, w indices */\0A#define VU_MSKIN_IN_AT 1  /* first item */\0A#define VU_MSKIN_OUT_AT 266  /* the output GIF tag */\0A#define VU_MSKIN_NREG 3\0A#define VU_MSKIN_REGS 0x512ull  /* GIF register list */\0A#define VU_MSKIN_NIN 5\0A#define VU_MSKIN_IN_POSITION 0  /* v3_16, 6 bytes */\0A#define VU_MSKIN_IN_NORMAL 1  /* v4_8, 4 bytes */\0A#define VU_MSKIN_IN_TEXCOORD 2  /* v2_16, 4 bytes */\0A#define VU_MSKIN_IN_JOINTS 3  /* v4_8u, 4 bytes */\0A#define VU_MSKIN_IN_WEIGHTS 4  /* v4_8u, 4 bytes */\0A#define VU_MSKIN_INPUTS {{0x9, 0, 6, 0x0}, {0xe, 0, 4, 0x0}, {0x5, 0, 4, 0x0}, {0xe, 1, 4, 0x0}, {0xe, 1, 4, 0x0}}  /* UNPACK format, unsigned, bytes, STMASK (fields 1.0 from COL) */\0A\0A", section ".vu.header", align 1
@llvm.used = appending global [1 x ptr] [ptr @vu.header], section "llvm.metadata"

; Function Attrs: mustprogress nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare float @llvm.maxnum.f32(float, float) #0

; Function Attrs: mustprogress nofree noinline norecurse noreturn nosync nounwind willreturn memory(readwrite, argmem: none, inaccessiblemem: none, target_mem: none)
define void @mskin() local_unnamed_addr #1 {
vu.entry:
  %ctx = tail call i16 @llvm.vu.xitop()
  %uni = shl i16 %ctx, 4
  %0 = add i16 %uni, 16
  %1 = add i16 %uni, 144
  %2 = inttoptr i16 %1 to ptr addrspace(1)
  %3 = addrspacecast ptr addrspace(1) %2 to ptr addrspace(12)
  %4 = inttoptr i16 %0 to ptr addrspace(1)
  %5 = addrspacecast ptr addrspace(1) %4 to ptr addrspace(12)
  %6 = load <16 x float>, ptr addrspace(12) %5, align 4, !tbaa !6, !alias.scope !8, !noalias !11
  %7 = add i16 %uni, 80
  %8 = inttoptr i16 %7 to ptr addrspace(1)
  %9 = addrspacecast ptr addrspace(1) %8 to ptr addrspace(12)
  %10 = load <3 x float>, ptr addrspace(12) %9, align 4, !alias.scope !8, !noalias !11
  %11 = add i16 %uni, 128
  %12 = inttoptr i16 %11 to ptr addrspace(1)
  %13 = addrspacecast ptr addrspace(1) %12 to ptr addrspace(12)
  %14 = load <4 x float>, ptr addrspace(12) %13, align 4, !alias.scope !8, !noalias !11
  %15 = add i16 %uni, 112
  %16 = inttoptr i16 %15 to ptr addrspace(1)
  %17 = addrspacecast ptr addrspace(1) %16 to ptr addrspace(12)
  %18 = load <4 x float>, ptr addrspace(12) %17, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %19 = add i16 %uni, 96
  %20 = inttoptr i16 %19 to ptr addrspace(1)
  %21 = addrspacecast ptr addrspace(1) %20 to ptr addrspace(12)
  %22 = load <4 x float>, ptr addrspace(12) %21, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %23 = shufflevector <16 x float> %6, <16 x float> poison, <4 x i32> <i32 12, i32 13, i32 14, i32 15>
  %24 = shufflevector <16 x float> %6, <16 x float> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 3>
  %25 = shufflevector <16 x float> %6, <16 x float> poison, <4 x i32> <i32 4, i32 5, i32 6, i32 7>
  %26 = shufflevector <16 x float> %6, <16 x float> poison, <4 x i32> <i32 8, i32 9, i32 10, i32 11>
  %27 = shufflevector <4 x float> %14, <4 x float> poison, <3 x i32> <i32 0, i32 1, i32 2>
  %28 = extractelement <4 x float> %14, i64 3
  br label %vu.batch

vu.batch:                                         ; preds = %vu.done, %vu.entry
  %top = tail call i16 @llvm.vu.xtop()
  %n = tail call i16 @llvm.vu.ilw(i16 %top, i32 0)
  %ctx7 = tail call i16 @llvm.vu.xitop()
  %tag = tail call <4 x float> @llvm.vu.lq(i16 %ctx7)
  %eop = tail call i16 @llvm.vu.ilw(i16 %ctx7, i32 0)
  %tag.x = or i16 %eop, %n
  %out = add i16 %top, 266
  %empty = icmp eq i16 %n, 0
  tail call void @llvm.vu.sq(<4 x float> %tag, i16 %out)
  tail call void @llvm.vu.isw(i16 %tag.x, i16 %out, i32 0)
  br i1 %empty, label %vu.done, label %vu.item.preheader

vu.item.preheader:                                ; preds = %vu.batch
  %o1 = add i16 %top, 267
  %in = add i16 %top, 1
  br label %vu.item

vu.item:                                          ; preds = %vu.item.preheader, %vu.item
  %i = phi i16 [ %i.next, %vu.item ], [ 0, %vu.item.preheader ]
  %pi = phi i16 [ %124, %vu.item ], [ %in, %vu.item.preheader ]
  %po = phi i16 [ %125, %vu.item ], [ %o1, %vu.item.preheader ]
  %29 = shl i16 %pi, 4
  %30 = inttoptr i16 %29 to ptr addrspace(2)
  %31 = add i16 %29, 16
  %32 = inttoptr i16 %31 to ptr addrspace(2)
  %33 = add i16 %29, 32
  %34 = inttoptr i16 %33 to ptr addrspace(2)
  %35 = add i16 %29, 48
  %36 = inttoptr i16 %35 to ptr addrspace(2)
  %37 = add i16 %29, 64
  %38 = inttoptr i16 %37 to ptr addrspace(2)
  %39 = shl i16 %po, 4
  %40 = inttoptr i16 %39 to ptr addrspace(3)
  %41 = add i16 %39, 16
  %42 = inttoptr i16 %41 to ptr addrspace(3)
  %43 = add i16 %39, 32
  %44 = inttoptr i16 %43 to ptr addrspace(3)
  %45 = addrspacecast ptr addrspace(2) %30 to ptr addrspace(7)
  %46 = load <4 x i32>, ptr addrspace(7) %45, align 4, !alias.scope !8, !noalias !16
  %47 = addrspacecast ptr addrspace(2) %32 to ptr addrspace(7)
  %48 = load <4 x i32>, ptr addrspace(7) %47, align 4, !alias.scope !8, !noalias !16
  %49 = addrspacecast ptr addrspace(2) %34 to ptr addrspace(7)
  %50 = load <2 x i32>, ptr addrspace(7) %49, align 4, !alias.scope !8, !noalias !16
  %51 = addrspacecast ptr addrspace(2) %36 to ptr addrspace(7)
  %52 = load <4 x i32>, ptr addrspace(7) %51, align 4, !alias.scope !8, !noalias !16
  %53 = addrspacecast ptr addrspace(2) %38 to ptr addrspace(7)
  %54 = load <4 x i32>, ptr addrspace(7) %53, align 4, !alias.scope !8, !noalias !16
  %conv.i = sitofp reassoc nnan ninf nsz arcp afn <4 x i32> %54 to <4 x float>
  %mul.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %conv.i, splat (float f0x3B808081)
  %mul1.i = shl <4 x i32> %52, splat (i32 2)
  %55 = bitcast <4 x i32> %mul1.i to <8 x i16>
  %56 = extractelement <8 x i16> %55, i64 0
  %arrayidx.i = getelementptr inbounds nuw [16 x i8], ptr addrspace(12) %3, i16 %56
  %57 = load <4 x float>, ptr addrspace(12) %arrayidx.i, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %splat.splat.i = shufflevector <4 x float> %mul.i, <4 x float> poison, <4 x i32> zeroinitializer
  %mul2.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %splat.splat.i, %57
  %58 = extractelement <8 x i16> %55, i64 2
  %arrayidx4.i = getelementptr inbounds nuw [16 x i8], ptr addrspace(12) %3, i16 %58
  %59 = load <4 x float>, ptr addrspace(12) %arrayidx4.i, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %splat.splat6.i = shufflevector <4 x float> %mul.i, <4 x float> poison, <4 x i32> <i32 1, i32 1, i32 1, i32 1>
  %mul7.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %splat.splat6.i, %59
  %add.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %mul2.i, %mul7.i
  %60 = extractelement <8 x i16> %55, i64 4
  %arrayidx9.i = getelementptr inbounds nuw [16 x i8], ptr addrspace(12) %3, i16 %60
  %61 = load <4 x float>, ptr addrspace(12) %arrayidx9.i, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %splat.splat11.i = shufflevector <4 x float> %mul.i, <4 x float> poison, <4 x i32> <i32 2, i32 2, i32 2, i32 2>
  %mul12.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %61, %splat.splat11.i
  %add13.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %add.i, %mul12.i
  %62 = extractelement <8 x i16> %55, i64 6
  %arrayidx15.i = getelementptr inbounds nuw [16 x i8], ptr addrspace(12) %3, i16 %62
  %63 = load <4 x float>, ptr addrspace(12) %arrayidx15.i, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %splat.splat17.i = shufflevector <4 x float> %mul.i, <4 x float> poison, <4 x i32> <i32 3, i32 3, i32 3, i32 3>
  %mul18.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %63, %splat.splat17.i
  %add19.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %add13.i, %mul18.i
  %arrayidx22.i = getelementptr inbounds nuw i8, ptr addrspace(12) %arrayidx.i, i16 16
  %64 = load <4 x float>, ptr addrspace(12) %arrayidx22.i, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %mul25.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %64, %splat.splat.i
  %arrayidx28.i = getelementptr inbounds nuw i8, ptr addrspace(12) %arrayidx4.i, i16 16
  %65 = load <4 x float>, ptr addrspace(12) %arrayidx28.i, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %mul31.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %65, %splat.splat6.i
  %add32.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %mul31.i, %mul25.i
  %arrayidx35.i = getelementptr inbounds nuw i8, ptr addrspace(12) %arrayidx9.i, i16 16
  %66 = load <4 x float>, ptr addrspace(12) %arrayidx35.i, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %mul38.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %66, %splat.splat11.i
  %add39.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %add32.i, %mul38.i
  %arrayidx42.i = getelementptr inbounds nuw i8, ptr addrspace(12) %arrayidx15.i, i16 16
  %67 = load <4 x float>, ptr addrspace(12) %arrayidx42.i, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %mul45.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %67, %splat.splat17.i
  %add46.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %add39.i, %mul45.i
  %arrayidx49.i = getelementptr inbounds nuw i8, ptr addrspace(12) %arrayidx.i, i16 32
  %68 = load <4 x float>, ptr addrspace(12) %arrayidx49.i, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %mul52.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %68, %splat.splat.i
  %arrayidx55.i = getelementptr inbounds nuw i8, ptr addrspace(12) %arrayidx4.i, i16 32
  %69 = load <4 x float>, ptr addrspace(12) %arrayidx55.i, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %mul58.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %69, %splat.splat6.i
  %add59.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %mul58.i, %mul52.i
  %arrayidx62.i = getelementptr inbounds nuw i8, ptr addrspace(12) %arrayidx9.i, i16 32
  %70 = load <4 x float>, ptr addrspace(12) %arrayidx62.i, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %mul65.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %70, %splat.splat11.i
  %add66.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %add59.i, %mul65.i
  %arrayidx69.i = getelementptr inbounds nuw i8, ptr addrspace(12) %arrayidx15.i, i16 32
  %71 = load <4 x float>, ptr addrspace(12) %arrayidx69.i, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %mul72.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %71, %splat.splat17.i
  %add73.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %add66.i, %mul72.i
  %arrayidx76.i = getelementptr inbounds nuw i8, ptr addrspace(12) %arrayidx.i, i16 48
  %72 = load <4 x float>, ptr addrspace(12) %arrayidx76.i, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %mul79.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %72, %splat.splat.i
  %arrayidx82.i = getelementptr inbounds nuw i8, ptr addrspace(12) %arrayidx4.i, i16 48
  %73 = load <4 x float>, ptr addrspace(12) %arrayidx82.i, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %mul85.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %73, %splat.splat6.i
  %arrayidx89.i = getelementptr inbounds nuw i8, ptr addrspace(12) %arrayidx9.i, i16 48
  %74 = load <4 x float>, ptr addrspace(12) %arrayidx89.i, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %mul92.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %74, %splat.splat11.i
  %arrayidx96.i = getelementptr inbounds nuw i8, ptr addrspace(12) %arrayidx15.i, i16 48
  %75 = load <4 x float>, ptr addrspace(12) %arrayidx96.i, align 4, !tbaa !15, !alias.scope !8, !noalias !11
  %mul99.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %75, %splat.splat17.i
  %conv102.i = sitofp reassoc nnan ninf nsz arcp afn <4 x i32> %48 to <4 x float>
  %conv.i224.i = sitofp reassoc nnan ninf nsz arcp afn <4 x i32> %46 to <4 x float>
  %mul.i225.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %conv.i224.i, splat (float f0x38000000)
  %splat.splat105.i = shufflevector <4 x float> %mul.i225.i, <4 x float> poison, <4 x i32> <i32 0, i32 0, i32 0, i32 poison>
  %mul106.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %add19.i, %splat.splat105.i
  %splat.splat108.i = shufflevector <4 x float> %mul.i225.i, <4 x float> poison, <4 x i32> <i32 1, i32 1, i32 1, i32 poison>
  %mul109.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %add46.i, %splat.splat108.i
  %splat.splat112.i = shufflevector <4 x float> %mul.i225.i, <4 x float> poison, <4 x i32> <i32 2, i32 2, i32 2, i32 poison>
  %mul113.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %add73.i, %splat.splat112.i
  %add86.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %mul109.i, %mul106.i
  %add93.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %add86.i, %mul79.i
  %add100.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %add93.i, %mul113.i
  %add110.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %add100.i, %mul85.i
  %add114.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %add110.i, %mul92.i
  %add115.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %add114.i, %mul99.i
  %splat.splat117.i = shufflevector <4 x float> %conv102.i, <4 x float> poison, <4 x i32> <i32 0, i32 0, i32 0, i32 poison>
  %mul118.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %add19.i, %splat.splat117.i
  %splat.splat120.i = shufflevector <4 x float> %conv102.i, <4 x float> poison, <4 x i32> <i32 1, i32 1, i32 1, i32 poison>
  %mul121.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %add46.i, %splat.splat120.i
  %add122.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %mul121.i, %mul118.i
  %splat.splat124.i = shufflevector <4 x float> %conv102.i, <4 x float> poison, <4 x i32> <i32 2, i32 2, i32 2, i32 poison>
  %mul125.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %add73.i, %splat.splat124.i
  %add126.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %add122.i, %mul125.i
  %76 = shufflevector <4 x float> %add126.i, <4 x float> poison, <3 x i32> <i32 0, i32 1, i32 2>
  %77 = shufflevector <4 x float> %add115.i, <4 x float> poison, <3 x i32> <i32 0, i32 1, i32 2>
  %.splat2 = shufflevector <4 x float> %add115.i, <4 x float> poison, <4 x i32> zeroinitializer
  %78 = fmul reassoc nnan ninf nsz arcp afn <4 x float> %.splat2, %24
  %79 = fadd nnan ninf nsz arcp afn <4 x float> %23, %78
  %.splat4 = shufflevector <4 x float> %add115.i, <4 x float> poison, <4 x i32> <i32 1, i32 1, i32 1, i32 1>
  %80 = fmul reassoc nnan ninf nsz arcp afn <4 x float> %.splat4, %25
  %81 = fadd nnan ninf nsz arcp afn <4 x float> %80, %79
  %.splat6 = shufflevector <4 x float> %add115.i, <4 x float> poison, <4 x i32> <i32 2, i32 2, i32 2, i32 2>
  %82 = fmul reassoc nnan ninf nsz arcp afn <4 x float> %.splat6, %26
  %83 = fadd nnan ninf nsz arcp afn <4 x float> %82, %81
  %84 = extractelement <4 x float> %83, i64 3
  %hlsl.rcp.i = fdiv reassoc nnan ninf nsz arcp afn float 1.000000e+00, %84
  %85 = fmul reassoc nnan ninf nsz arcp afn <3 x float> %76, %76
  %86 = extractelement <3 x float> %85, i64 0
  %87 = extractelement <3 x float> %85, i64 1
  %88 = extractelement <3 x float> %85, i64 2
  %89 = fadd reassoc nnan ninf nsz arcp afn float %87, f0x358637BD
  %90 = fadd reassoc nnan ninf nsz arcp afn float %89, %86
  %add132.i = fadd reassoc nnan ninf nsz arcp afn float %90, %88
  %91 = tail call reassoc nnan ninf nsz arcp afn float @llvm.sqrt.f32(float %add132.i)
  %92 = fdiv reassoc nnan ninf nsz arcp afn float 1.000000e+00, %91
  %splat.splatinsert133.i = insertelement <3 x float> poison, float %92, i64 0
  %splat.splat134.i = shufflevector <3 x float> %splat.splatinsert133.i, <3 x float> poison, <3 x i32> zeroinitializer
  %mul135.i = fmul reassoc nnan ninf nsz arcp afn <3 x float> %splat.splat134.i, %76
  %93 = fmul reassoc nnan ninf nsz arcp afn <3 x float> %mul135.i, %10
  %94 = tail call reassoc nnan ninf nsz arcp afn float @llvm.vector.reduce.fadd.v3f32(float 0.000000e+00, <3 x float> %93)
  %mul.i228.i = fmul reassoc nnan ninf nsz arcp afn float %94, 5.000000e-01
  %add.i.i = fadd reassoc nnan ninf nsz arcp afn float %mul.i228.i, 5.000000e-01
  %mul1.i.i = fmul reassoc nnan ninf nsz arcp afn float %add.i.i, %add.i.i
  %sub.i = fsub reassoc nnan ninf nsz arcp afn <3 x float> %27, %77
  %95 = fmul reassoc nnan ninf nsz arcp afn <3 x float> %sub.i, %sub.i
  %96 = extractelement <3 x float> %95, i64 0
  %97 = extractelement <3 x float> %95, i64 1
  %98 = extractelement <3 x float> %95, i64 2
  %99 = fadd reassoc nnan ninf nsz arcp afn float %97, f0x2B8CBCCC
  %100 = fadd reassoc nnan ninf nsz arcp afn float %99, %96
  %add139.i = fadd reassoc nnan ninf nsz arcp afn float %100, %98
  %101 = tail call reassoc nnan ninf nsz arcp afn float @llvm.sqrt.f32(float %add139.i)
  %102 = fdiv reassoc nnan ninf nsz arcp afn float 1.000000e+00, %101
  %splat.splatinsert141.i = insertelement <3 x float> poison, float %102, i64 0
  %splat.splat142.i = shufflevector <3 x float> %splat.splatinsert141.i, <3 x float> poison, <3 x i32> zeroinitializer
  %mul143.i = fmul reassoc nnan ninf nsz arcp afn <3 x float> %splat.splat142.i, %sub.i
  %add.i229.i = fadd reassoc nnan ninf nsz arcp afn <3 x float> %mul143.i, %10
  %103 = fmul reassoc nnan ninf nsz arcp afn <3 x float> %add.i229.i, %mul135.i
  %104 = tail call reassoc nnan ninf nsz arcp afn float @llvm.vector.reduce.fadd.v3f32(float 0.000000e+00, <3 x float> %103)
  %105 = fmul reassoc nnan ninf nsz arcp afn <3 x float> %add.i229.i, %add.i229.i
  %106 = extractelement <3 x float> %105, i64 0
  %107 = extractelement <3 x float> %105, i64 1
  %108 = extractelement <3 x float> %105, i64 2
  %109 = fadd reassoc nnan ninf nsz arcp afn float %107, f0x2B8CBCCC
  %110 = fadd reassoc nnan ninf nsz arcp afn float %109, %106
  %add2.i.i = fadd reassoc nnan ninf nsz arcp afn float %110, %108
  %111 = tail call reassoc nnan ninf nsz arcp afn float @llvm.sqrt.f32(float %add2.i.i)
  %mul.i230.i = fdiv reassoc nnan ninf nsz arcp afn float %104, %111
  %elt.max.i.i = tail call reassoc nnan ninf nsz arcp afn float @llvm.maxnum.f32(float %mul.i230.i, float 0.000000e+00)
  %mul3.i.i = fmul reassoc nnan ninf nsz arcp afn float %elt.max.i.i, %elt.max.i.i
  %mul4.i.i = fmul reassoc nnan ninf nsz arcp afn float %mul3.i.i, %mul3.i.i
  %mul5.i.i = fmul reassoc nnan ninf nsz arcp afn float %mul4.i.i, %mul4.i.i
  %112 = fmul reassoc nnan ninf nsz arcp afn float %mul5.i.i, %mul5.i.i
  %mul145.i = fmul reassoc nnan ninf nsz arcp afn float %112, %28
  %splat.splatinsert146.i = insertelement <4 x float> poison, float %mul1.i.i, i64 0
  %splat.splat147.i = shufflevector <4 x float> %splat.splatinsert146.i, <4 x float> poison, <4 x i32> <i32 0, i32 0, i32 0, i32 poison>
  %mul148.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %splat.splat147.i, %22
  %add149.i = fadd reassoc nnan ninf nsz arcp afn <4 x float> %mul148.i, %18
  %.scalar = fmul reassoc nnan ninf nsz arcp afn float %hlsl.rcp.i, f0x39800000
  %113 = insertelement <2 x float> poison, float %.scalar, i64 0
  %114 = shufflevector <2 x float> %113, <2 x float> poison, <4 x i32> <i32 0, i32 0, i32 poison, i32 poison>
  %115 = shufflevector <2 x i32> %50, <2 x i32> poison, <4 x i32> <i32 0, i32 1, i32 poison, i32 poison>
  %116 = sitofp reassoc nnan ninf nsz arcp afn <4 x i32> %115 to <4 x float>
  %117 = fmul reassoc nnan ninf nsz arcp afn <4 x float> %114, %116
  %vecinit160.i = insertelement <4 x float> %117, float %hlsl.rcp.i, i64 2
  %vecinit167.i = insertelement <4 x float> %add149.i, float %mul145.i, i64 3
  %elt.min.i = tail call reassoc nnan ninf nsz arcp afn <4 x float> @llvm.minnum.v4f32(<4 x float> %vecinit167.i, <4 x float> splat (float 2.550000e+02))
  %conv168.i = fptosi <4 x float> %elt.min.i to <4 x i32>
  %.scalar7 = fmul reassoc nnan ninf nsz arcp afn float %hlsl.rcp.i, 1.600000e+01
  %118 = insertelement <4 x float> poison, float %.scalar7, i64 0
  %119 = shufflevector <4 x float> %118, <4 x float> poison, <4 x i32> zeroinitializer
  %mul.i.i = fmul reassoc nnan ninf nsz arcp afn <4 x float> %119, %83
  %conv.i.i = fptosi <4 x float> %mul.i.i to <4 x i32>
  %120 = addrspacecast ptr addrspace(3) %40 to ptr addrspace(8)
  %121 = shufflevector <4 x float> %vecinit160.i, <4 x float> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 poison>
  store <4 x float> %121, ptr addrspace(8) %120, align 4, !alias.scope !16, !noalias !8
  %122 = addrspacecast ptr addrspace(3) %42 to ptr addrspace(8)
  store <4 x i32> %conv168.i, ptr addrspace(8) %122, align 4, !alias.scope !16, !noalias !8
  %123 = addrspacecast ptr addrspace(3) %44 to ptr addrspace(8)
  store <4 x i32> %conv.i.i, ptr addrspace(8) %123, align 4, !alias.scope !16, !noalias !8
  %i.next = add nuw i16 %i, 1
  %124 = add i16 %pi, 5
  %125 = add i16 %po, 3
  %last = icmp eq i16 %i.next, %n
  br i1 %last, label %vu.done, label %vu.item

vu.done:                                          ; preds = %vu.item, %vu.batch
  tail call void @llvm.vu.xgkick(i16 %out)
  tail call void @llvm.vu.yield()
  br label %vu.batch
}

; Function Attrs: mustprogress nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare <4 x float> @llvm.minnum.v4f32(<4 x float>, <4 x float>) #0

; Function Attrs: mustprogress nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare float @llvm.sqrt.f32(float) #0

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

; Function Attrs: nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare float @llvm.vector.reduce.fadd.v3f32(float, <3 x float>) #6

attributes #0 = { mustprogress nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none) }
attributes #1 = { mustprogress nofree noinline norecurse noreturn nosync nounwind willreturn memory(readwrite, argmem: none, inaccessiblemem: none, target_mem: none) "no-signed-zeros-fp-math"="false" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="vu1" "vu-header-prefix"="VU_" }
attributes #2 = { mustprogress nocallback nofree nosync nounwind willreturn memory(inaccessiblemem: readwrite) }
attributes #3 = { mustprogress nocallback nofree nosync nounwind willreturn memory(read) }
attributes #4 = { mustprogress nocallback nofree nosync nounwind willreturn }
attributes #5 = { mustprogress nocallback nofree nosync nounwind willreturn memory(write) }
attributes #6 = { nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none) }

!llvm.errno.tbaa = !{!1}

!0 = !{!"clang"}
!1 = !{!2, !3, i64 0}
!2 = !{!"__libc_errno", !3, i64 0}
!3 = !{!"int", !4, i64 0}
!4 = !{!"omnipotent char", !5, i64 0}
!5 = !{!"Simple C++ TBAA"}
!6 = !{!7, !7, i64 0}
!7 = !{!"float", !4, i64 0}
!8 = !{!9}
!9 = distinct !{!9, !10, !"in"}
!10 = distinct !{!10, i1 false, !"mskin"}
!11 = !{!12, !14}
!12 = distinct !{!12, !13, !"_Z5mskin2In: %agg.result"}
!13 = distinct !{!13, i1 true, !"_Z5mskin2In"}
!14 = distinct !{!14, !10, !"out"}
!15 = !{!4, !4, i64 0}
!16 = !{!14}
