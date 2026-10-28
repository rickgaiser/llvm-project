; REQUIRES: asserts
; RUN: llc -mtriple=mips64el -mcpu=r5900 -target-abi=n32 -debug-only=mips-subtarget -o /dev/null %s 2>&1 | FileCheck %s --check-prefix=R5900
; RUN: llc -mtriple=mips64el -mcpu=mips3 -target-abi=n32 -debug-only=mips-subtarget -o /dev/null %s 2>&1 | FileCheck %s --check-prefix=MIPS3 --allow-empty
; Medium-sized computed-goto dispatches avoid cross-block register coalescing.
; Ordinary small functions and non-R5900 CPUs keep their existing policy.
; R5900-NOT: R5900 coalescing: skip global copies for small_dispatch
; R5900: R5900 coalescing: skip global copies for medium_dispatch
; MIPS3-NOT: R5900 coalescing: skip global copies for
define i32 @small_dispatch(ptr %target) {
entry:
  indirectbr ptr %target, [label %a, label %b]
a:
  ret i32 1
b:
  ret i32 2
}

define i32 @medium_dispatch(ptr %target) {
entry:
  indirectbr ptr %target, [label %L0, label %L1, label %L2, label %L3, label %L4, label %L5, label %L6, label %L7, label %L8, label %L9, label %L10, label %L11, label %L12, label %L13, label %L14, label %L15, label %L16, label %L17, label %L18, label %L19, label %L20, label %L21, label %L22, label %L23, label %L24, label %L25, label %L26, label %L27, label %L28, label %L29, label %L30, label %L31, label %L32, label %L33, label %L34, label %L35, label %L36, label %L37, label %L38, label %L39, label %L40, label %L41, label %L42, label %L43, label %L44, label %L45, label %L46, label %L47, label %L48, label %L49, label %L50, label %L51, label %L52, label %L53, label %L54, label %L55, label %L56, label %L57, label %L58, label %L59, label %L60, label %L61, label %L62, label %L63, label %L64, label %L65, label %L66, label %L67, label %L68, label %L69]
L0:
  ret i32 0
L1:
  ret i32 1
L2:
  ret i32 2
L3:
  ret i32 3
L4:
  ret i32 4
L5:
  ret i32 5
L6:
  ret i32 6
L7:
  ret i32 7
L8:
  ret i32 8
L9:
  ret i32 9
L10:
  ret i32 10
L11:
  ret i32 11
L12:
  ret i32 12
L13:
  ret i32 13
L14:
  ret i32 14
L15:
  ret i32 15
L16:
  ret i32 16
L17:
  ret i32 17
L18:
  ret i32 18
L19:
  ret i32 19
L20:
  ret i32 20
L21:
  ret i32 21
L22:
  ret i32 22
L23:
  ret i32 23
L24:
  ret i32 24
L25:
  ret i32 25
L26:
  ret i32 26
L27:
  ret i32 27
L28:
  ret i32 28
L29:
  ret i32 29
L30:
  ret i32 30
L31:
  ret i32 31
L32:
  ret i32 32
L33:
  ret i32 33
L34:
  ret i32 34
L35:
  ret i32 35
L36:
  ret i32 36
L37:
  ret i32 37
L38:
  ret i32 38
L39:
  ret i32 39
L40:
  ret i32 40
L41:
  ret i32 41
L42:
  ret i32 42
L43:
  ret i32 43
L44:
  ret i32 44
L45:
  ret i32 45
L46:
  ret i32 46
L47:
  ret i32 47
L48:
  ret i32 48
L49:
  ret i32 49
L50:
  ret i32 50
L51:
  ret i32 51
L52:
  ret i32 52
L53:
  ret i32 53
L54:
  ret i32 54
L55:
  ret i32 55
L56:
  ret i32 56
L57:
  ret i32 57
L58:
  ret i32 58
L59:
  ret i32 59
L60:
  ret i32 60
L61:
  ret i32 61
L62:
  ret i32 62
L63:
  ret i32 63
L64:
  ret i32 64
L65:
  ret i32 65
L66:
  ret i32 66
L67:
  ret i32 67
L68:
  ret i32 68
L69:
  ret i32 69
}
