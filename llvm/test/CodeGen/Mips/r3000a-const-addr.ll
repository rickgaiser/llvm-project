; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a < %s | FileCheck %s --check-prefix=FOLD
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=mips1 -mattr=+const-addr-fold < %s | FileCheck %s --check-prefix=FOLD
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=mips1 < %s | FileCheck %s --check-prefix=NOFOLD
; RUN: llc -mtriple=mipsel-unknown-elf -mcpu=r3000a -mattr=-const-addr-fold < %s | FileCheck %s --check-prefix=NOFOLD

; The R3000A puts the low 16 bits of a constant address in the load or store
; offset: one lui instead of lui, ori and a zero offset. Other targets keep the
; separate ori unless const-addr-fold is requested.

define i32 @load_reg() {
; FOLD-LABEL: load_reg:
; FOLD:       lui $1, 49037
; FOLD-NEXT:  lw $2, -32152($1)
;
; NOFOLD-LABEL: load_reg:
; NOFOLD:       lui $1, 49036
; NOFOLD-NEXT:  ori $1, $1, 33384
; NOFOLD-NEXT:  lw $2, 0($1)
  %v = load volatile i32, ptr inttoptr (i32 -1081310616 to ptr)
  ret i32 %v
}

define void @store_reg(i32 %x) {
; FOLD-LABEL: store_reg:
; FOLD:       lui $1, 49037
; FOLD:       sw $4, -32152($1)
;
; NOFOLD-LABEL: store_reg:
; NOFOLD:       lui $1, 49036
; NOFOLD-NEXT:  ori $1, $1, 33384
  store volatile i32 %x, ptr inttoptr (i32 -1081310616 to ptr)
  ret void
}

define i32 @load_low() {
; FOLD-LABEL: load_low:
; FOLD:       lw $2, 16($zero)
;
; NOFOLD-LABEL: load_low:
; NOFOLD:       addiu $1, $zero, 16
; NOFOLD-NEXT:  lw $2, 0($1)
  %v = load volatile i32, ptr inttoptr (i32 16 to ptr)
  ret i32 %v
}
