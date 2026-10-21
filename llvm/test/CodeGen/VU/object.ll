; RUN: llc -mtriple=vu -filetype=obj < %s -o %t.o
; RUN: llvm-readobj -h -S %t.o | FileCheck %s --check-prefix=ELF
; RUN: llvm-objdump -d --triple=vu %t.o 2>/dev/null | FileCheck %s --check-prefix=DIS

; Generated code goes in .vutext of a dvp-as compatible object.

; ELF: Machine: EM_MIPS
; ELF: Flags [ (0x20920024)
; ELF: Name: .vutext
; ELF: AddressAlignment: 8

; DIS:      <kernel>:
; DIS-NEXT:   0: 3c 03 00 80 ff 02 00 40  nop[e] nop
; DIS-NEXT:   8: 3c 03 00 80 ff 02 00 00  nop nop

define void @kernel() {
  ret void
}
