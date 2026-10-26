; RUN: opt -mtriple=vu -passes=vu-hlsl-adapter -S < %s | FileCheck %s

; A field value after the format suffix of an input's semantic
; (int3 uv : TEXCOORD_V2_16_F4096): the format is v2_16_f4096, and the header
; names the input by the semantic without either suffix.

; CHECK: @texcoord = external addrspace(2) global <3 x i32>, !vu.input [[UV:![0-9]+]]
; CHECK: [[UV]] = !{i32 1, !"v2_16_f4096"}

target triple = "spirv-unknown-vulkan1.3-vertex"

@POSITION0 = external hidden thread_local local_unnamed_addr addrspace(7) externally_initialized constant <3 x float>, !spirv.Decorations !0
@TEXCOORD_V2_16_F4096 = external hidden thread_local local_unnamed_addr addrspace(7) externally_initialized constant <3 x i32>, !spirv.Decorations !2
@GIF_ST = external hidden thread_local local_unnamed_addr addrspace(8) global <4 x float>, !spirv.Decorations !0
@GIF_XYZ2 = external hidden thread_local local_unnamed_addr addrspace(8) global <4 x i32>, !spirv.Decorations !2

define void @vs() local_unnamed_addr #0 {
  %p = load <3 x float>, ptr addrspace(7) @POSITION0, align 4
  %t = load <3 x i32>, ptr addrspace(7) @TEXCOORD_V2_16_F4096, align 4
  %t4 = shufflevector <3 x i32> %t, <3 x i32> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 poison>
  %tf = sitofp <4 x i32> %t4 to <4 x float>
  %p4 = shufflevector <3 x float> %p, <3 x float> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 poison>
  %i = fptosi <4 x float> %p4 to <4 x i32>
  store <4 x float> %tf, ptr addrspace(8) @GIF_ST, align 4
  store <4 x i32> %i, ptr addrspace(8) @GIF_XYZ2, align 4
  ret void
}

attributes #0 = { "hlsl.shader"="vertex" }

!0 = !{!1}
!1 = !{i32 30, i32 0}
!2 = !{!3}
!3 = !{i32 30, i32 1}
