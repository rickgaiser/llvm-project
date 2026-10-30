// RUN: %clang_cc1 -triple mips64el-unknown-none -target-cpu r5900 -target-abi n32 -O3 -mllvm -mips-r5900-state-dispatch-opts -emit-llvm -o - %s | FileCheck %s --check-prefix=R5900
// RUN: %clang_cc1 -triple mips64el-unknown-none -target-cpu r5900 -target-abi n32 -O3 -emit-llvm -o - %s | FileCheck %s --check-prefix=GENERIC
// RUN: %clang_cc1 -triple mips64el-unknown-none -target-cpu r5900 -target-abi n32 -O2 -emit-llvm -o - %s | FileCheck %s --check-prefix=GENERIC
// RUN: %clang_cc1 -triple mips64el-unknown-none -target-cpu r5900 -target-abi n32 -Os -emit-llvm -o - %s | FileCheck %s --check-prefix=GENERIC
// RUN: %clang_cc1 -triple mips64el-unknown-none -target-cpu mips64r2 -target-abi n32 -O3 -emit-llvm -o - %s | FileCheck %s --check-prefix=GENERIC

// A call-free, fixed four-iteration loop with a multiway branch may be worth
// fully unrolling on the R5900 with an explicitly enabled experimental budget.
// Without the flag, retain normal unrolling at O3 and on other CPUs. Volatile
// loads keep the four alternatives distinct after IR optimization.

// R5900-LABEL: define{{.*}} @test_switch_four(
// R5900-COUNT-4: switch i8
// R5900-NOT: !llvm.loop
// GENERIC-LABEL: define{{.*}} @test_switch_four(
// GENERIC: switch i8
// GENERIC: !llvm.loop

unsigned test_switch_four(const unsigned char *state, volatile unsigned *out,
                          unsigned x) {
  for (unsigned i = 0; i < 4; ++i) {
    switch (state[i]) {
    case 0: {
      unsigned a = x;
      a += out[i + 0];
      a += out[i + 1];
      a += out[i + 2];
      a += out[i + 3];
      a += out[i + 4];
      a += out[i + 5];
      a += out[i + 6];
      out[i] = a;
      break;
    }
    case 1: {
      unsigned a = x;
      a ^= out[i + 0] + 11;
      a ^= out[i + 1] + 12;
      a ^= out[i + 2] + 13;
      a ^= out[i + 3] + 14;
      a ^= out[i + 4] + 15;
      a ^= out[i + 5] + 16;
      out[i] = a;
      break;
    }
    case 2: {
      unsigned a = x;
      a = a * 33u ^ out[i + 0];
      a = a * 33u ^ out[i + 1];
      a = a * 33u ^ out[i + 2];
      a = a * 33u ^ out[i + 3];
      a = a * 33u ^ out[i + 4];
      a = a * 33u ^ out[i + 5];
      out[i] = a;
      break;
    }
    case 3: {
      unsigned a = x;
      a = (a >> 1) + out[i + 0] * 3u;
      a = (a >> 1) + out[i + 1] * 3u;
      a = (a >> 1) + out[i + 2] * 3u;
      a = (a >> 1) + out[i + 3] * 3u;
      a = (a >> 1) + out[i + 4] * 3u;
      a = (a >> 1) + out[i + 5] * 3u;
      out[i] = a;
      break;
    }
    }
  }
  return out[0];
}
