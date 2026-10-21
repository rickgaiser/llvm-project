; RUN: llvm-mc -triple=vu -show-encoding %s | FileCheck %s --check-prefix=ASM
; RUN: llvm-mc -triple=vu -filetype=obj %s -o %t.o
; RUN: llvm-objdump -s -j .vutext %t.o 2>/dev/null | FileCheck %s --check-prefix=BYTES
; RUN: llvm-readobj -r %t.o | FileCheck %s --check-prefix=RELOC

; Branch offsets count micro instruction words from the next word. A plain
; integer is used as the raw offset, like dvp-as does.

.vu
start:
  nop    ibne vi1, vi2, start
  nop    b next
next:
  nop    bal vi15, start
  nop    b 0x30
  nop    b -1
; Undefined symbols get an R_MIPS_DVP_11_PCREL (120) relocation with the
; addend stored in the instruction (-1 for the symbol itself), as in dvp-as.
  nop    b extern_label
  nop[e] nop
  nop    nop

; ASM:      nop ibne vi1, vi2, start ; encoding: [A,0b00010AAA,0x01,0x52,0xff,0x02,0x00,0x00]
; ASM-NEXT: ; fixup A - offset: 0, value: start, kind: fixup_vu_pcrel11
; ASM:      nop b 48 ; encoding: [0x30,0x00,0x00,0x40,0xff,0x02,0x00,0x00]
; ASM-NEXT: nop b -1 ; encoding: [0xff,0x07,0x00,0x40,0xff,0x02,0x00,0x00]

; BYTES:      0000 ff170152 ff020000 00000040 ff020000
; BYTES-NEXT: 0010 fd070f42 ff020000 30000040 ff020000
; BYTES-NEXT: 0020 ff070040 ff020000 ff070040 ff020000
; BYTES-NEXT: 0030 3c030080 ff020040 3c030080 ff020000

; RELOC:      Section ({{[0-9]+}}) .rel.vutext {
; RELOC-NEXT:   0x28 Unknown extern_label
; RELOC-NEXT: }
