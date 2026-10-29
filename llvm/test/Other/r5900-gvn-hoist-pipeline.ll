; Verify that the R5900 -O3 pipeline hoists equivalent loads before codegen,
; while generic MIPS, other architectures, and lower optimization levels keep
; their existing pipeline. The explicit GVN hoist option remains supported.
;
; RUN: opt -mtriple=mips64el-unknown-none -mcpu=r5900 -passes='default<O3>' -print-pipeline-passes -disable-output %s | FileCheck %s --check-prefix=R5900
; RUN: opt -mtriple=mips64el-unknown-none -mcpu=r5900 -passes='default<O2>' -print-pipeline-passes -disable-output %s | not grep -q 'gvn-hoist'
; RUN: opt -mtriple=mips64el-unknown-none -mcpu=mips64r2 -passes='default<O3>' -print-pipeline-passes -disable-output %s | not grep -q 'gvn-hoist'
; RUN: opt -mtriple=x86_64-unknown-linux-gnu -mcpu=x86-64 -passes='default<O3>' -print-pipeline-passes -disable-output %s | not grep -q 'gvn-hoist'
; RUN: opt -mtriple=mips64el-unknown-none -mcpu=mips64r2 -enable-gvn-hoist -passes='default<O3>' -print-pipeline-passes -disable-output %s | FileCheck %s --check-prefix=R5900
;
; R5900: gvn-hoist

define i32 @id(i32 %x) {
entry:
  ret i32 %x
}
