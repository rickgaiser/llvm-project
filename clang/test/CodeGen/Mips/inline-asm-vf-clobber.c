// RUN: %clang_cc1 -triple mips64el-unknown-linux-gnuabin32 -target-cpu r5900 \
// RUN:   -target-feature +vu0 -emit-llvm -o - %s | FileCheck %s

// The R5900's VU0 vector registers can be named in a clobber list: a VU0
// micro program started from inline asm shares them.

// CHECK-LABEL: @start
// CHECK: call void asm sideeffect "", "~{$vf1},~{$vf20},~{$vf31},~{memory}{{.*}}"()
void start(void) { __asm__ volatile("" ::: "$vf1", "$vf20", "$vf31", "memory"); }
