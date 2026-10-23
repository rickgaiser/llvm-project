; RUN: llc -mtriple=mips64el -mcpu=r5900 -target-abi n32 -show-mc-encoding < %s \
; RUN:   | FileCheck %s

; The R5900 reads the operand of SQRT.S from the ft field (binutils:
; "sqrt.s D,T"); with it in fs, as in MIPS II, it takes the root of $f0.
define float @sqrt_s(float %x) {
; CHECK-LABEL: sqrt_s:
; CHECK: sqrt.s $f0, $f12 # encoding: [0x04,0x00,0x0c,0x46]
  %r = call nnan ninf float @llvm.sqrt.f32(float %x)
  ret float %r
}
