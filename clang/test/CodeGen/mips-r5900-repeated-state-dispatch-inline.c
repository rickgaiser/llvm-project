// RUN: %clang_cc1 -triple mips64el-unknown-none -target-cpu r5900 -target-abi n32 -O3 -emit-llvm -o - %s | FileCheck %s --check-prefix=R5900
// RUN: %clang_cc1 -triple mips64el-unknown-none -target-cpu r5900 -target-abi n32 -O2 -emit-llvm -o - %s | FileCheck %s --check-prefix=GENERIC
// RUN: %clang_cc1 -triple mips64el-unknown-none -target-cpu r5900 -target-abi n32 -Os -emit-llvm -o - %s | FileCheck %s --check-prefix=GENERIC
// RUN: %clang_cc1 -triple mips64el-unknown-none -target-cpu r5900 -target-abi n32 -Oz -emit-llvm -o - %s | FileCheck %s --check-prefix=GENERIC
// RUN: %clang_cc1 -triple mips64el-unknown-none -target-cpu mips64r2 -target-abi n32 -O3 -emit-llvm -o - %s | FileCheck %s --check-prefix=GENERIC
// RUN: %clang_cc1 -triple mips64el-unknown-none -target-cpu r5900 -target-abi n32 -O3 -DFIVE_CALLS -emit-llvm -o - %s | FileCheck %s --check-prefix=FIVE

// On R5900 O3, repeated calls to a local, call-free state dispatcher with
// four multiway branches can be more profitable to inline after unrolling.
// Other MIPS CPUs and lower optimization levels retain the outlined helper.
// R5900-LABEL: define{{.*}} @outer_loop(
// R5900-NOT: @dispatch4(
// R5900: ret void
// GENERIC-LABEL: define{{.*}} @outer_loop(
// GENERIC-COUNT-4: call fastcc void @dispatch4(
// GENERIC: ret void
// FIVE-LABEL: define{{.*}} @outer_loop(
// FIVE-COUNT-5: call fastcc void @dispatch4(
// FIVE: ret void

static void dispatch4(const unsigned char *state, volatile unsigned *out) {
  for (unsigned i=0;i<4;i++) {
    switch (state[i]) {
      case 0: {
        unsigned a = out[i];
        a = a + out[i + 0];
        a = a + out[i + 1];
        a = a + out[i + 2];
        a = a + out[i + 3];
        a = a + out[i + 4];
        a = a + out[i + 5];
        a = a + out[i + 6];
        a = a + out[i + 7];
        a = a + out[i + 8];
        out[i] = a;
        break;
      }
      case 1: {
        unsigned a = out[i];
        a = a ^ (out[i + 0] + 11);
        a = a ^ (out[i + 1] + 12);
        a = a ^ (out[i + 2] + 13);
        a = a ^ (out[i + 3] + 14);
        a = a ^ (out[i + 4] + 15);
        a = a ^ (out[i + 5] + 16);
        a = a ^ (out[i + 6] + 17);
        a = a ^ (out[i + 7] + 18);
        a = a ^ (out[i + 8] + 19);
        out[i] = a;
        break;
      }
      case 2: {
        unsigned a = out[i];
        a = a * 33u ^ out[i + 0];
        a = a * 33u ^ out[i + 1];
        a = a * 33u ^ out[i + 2];
        a = a * 33u ^ out[i + 3];
        a = a * 33u ^ out[i + 4];
        a = a * 33u ^ out[i + 5];
        a = a * 33u ^ out[i + 6];
        a = a * 33u ^ out[i + 7];
        a = a * 33u ^ out[i + 8];
        out[i] = a;
        break;
      }
      case 3: {
        unsigned a = out[i];
        a = (a >> 1) + out[i + 0] * 3u;
        a = (a >> 1) + out[i + 1] * 3u;
        a = (a >> 1) + out[i + 2] * 3u;
        a = (a >> 1) + out[i + 3] * 3u;
        a = (a >> 1) + out[i + 4] * 3u;
        a = (a >> 1) + out[i + 5] * 3u;
        a = (a >> 1) + out[i + 6] * 3u;
        a = (a >> 1) + out[i + 7] * 3u;
        a = (a >> 1) + out[i + 8] * 3u;
        out[i] = a;
        break;
      }
    }
  }
}
void outer_loop(const unsigned char *state, volatile unsigned *out, unsigned count) {
  while (count--) {
    dispatch4(state, out);
    dispatch4(state + 4, out + 4);
    dispatch4(state + 8, out + 8);
    dispatch4(state + 12, out + 12);
#ifdef FIVE_CALLS
    dispatch4(state + 16, out + 16);
#endif
  }
}
