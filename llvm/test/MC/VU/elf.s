; RUN: llvm-mc -triple=vu -filetype=obj %s -o %t.o
; RUN: llvm-readobj -h -S %t.o | FileCheck %s

; VU objects are 32-bit little-endian MIPS ELF files with the e_flags of
; dvp-as, so they link with Emotion Engine code. ".vu" switches to .vutext.

; CHECK:      Class: 32-bit
; CHECK:      DataEncoding: LittleEndian
; CHECK:      Machine: EM_MIPS
; CHECK:      Flags [ (0x20920024)
; CHECK:        EF_MIPS_ABI2
; CHECK:        EF_MIPS_ARCH_3
; CHECK:        EF_MIPS_CPIC
; CHECK:        EF_MIPS_MACH_5900
; CHECK:      Name: .vutext
; CHECK-NEXT: Type: SHT_PROGBITS
; CHECK-NEXT: Flags [
; CHECK-NEXT:   SHF_ALLOC
; CHECK-NEXT:   SHF_EXECINSTR
; CHECK-NEXT: ]
; CHECK-NEXT: Address: 0x0
; CHECK-NEXT: Offset:
; CHECK-NEXT: Size: 32
; CHECK-NEXT: Link: 0
; CHECK-NEXT: Info: 0
; CHECK-NEXT: AddressAlignment: 16

.vu
  nop    nop
; ".align 4" aligns to 16 bytes and pads with zero words, like dvp-as.
  .align 4
  nop[e] nop
  nop    nop
