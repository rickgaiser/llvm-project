; RUN: llc -mtriple=dvpvu -mcpu=vu1 < %s | FileCheck %s
; Test EFU operations (VU1 only)

; XFAIL: *
; TODO: Implement EFU intrinsics and lowering

; EFU operations are available only on VU1 and use the P register.
; Similar to division, they require:
; 1. EFU instruction (esin, ecos, etc.) - starts the computation
; 2. WAITP - waits for P register to be ready
; 3. Use P register result

; These would likely be exposed as target intrinsics:
; declare float @llvm.dvpvu.esin.f32(float)
; declare float @llvm.dvpvu.ecos.f32(float)
; declare float @llvm.dvpvu.eexp.f32(float)
; declare float @llvm.dvpvu.elog.f32(float)
; declare float @llvm.dvpvu.esqrt.f32(float)
; declare float @llvm.dvpvu.ersqrt.f32(float)
; declare float @llvm.dvpvu.eatan.f32(float)

define void @placeholder() {
  ret void
}
