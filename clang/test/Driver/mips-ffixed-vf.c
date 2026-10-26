// -ffixed-vfN reserves VU0 register vfN (R5900).
// RUN: %clang --target=mips64el -mcpu=r5900 -mabi=n32 -mvu0 -ffixed-vf1 \
// RUN:   -ffixed-vf12 -ffixed-vf31 -### -c %s 2>&1 | FileCheck %s
// CHECK: "-target-feature" "+reserve-vf1"
// CHECK: "-target-feature" "+reserve-vf12"
// CHECK: "-target-feature" "+reserve-vf31"

