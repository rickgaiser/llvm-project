; REQUIRES: asserts
; RUN: llc -mtriple=mips64el -mcpu=r5900 -target-abi=n32 -O2 -debug-only=mips-subtarget -o /dev/null %s 2>&1 | FileCheck %s --check-prefix=R5900
; RUN: llc -mtriple=mips64el -mcpu=mips3 -target-abi=n32 -O2 -debug-only=mips-subtarget -o /dev/null %s 2>&1 | FileCheck %s --check-prefix=MIPS3 --allow-empty
;
; A compact PHI-heavy loop must use local copy coalescing only on R5900;
; a similar loop with few PHIs and generic MIPS keep their usual policy.
; R5900: R5900 coalescing: skip global copies for PHI-rich phi_heavy_loop
; R5900-NOT: R5900 coalescing: skip global copies for PHI-rich phi_light_loop
; MIPS3-NOT: R5900 coalescing:
declare void @observe(i32)

define void @phi_heavy_loop(i32 %n) {
entry:
  br label %loop
loop:
  %p0 = phi i32 [0, %entry], [%n, %latch]
  %p1 = phi i32 [1, %entry], [%n, %latch]
  %p2 = phi i32 [2, %entry], [%n, %latch]
  %p3 = phi i32 [3, %entry], [%n, %latch]
  %p4 = phi i32 [4, %entry], [%n, %latch]
  %p5 = phi i32 [5, %entry], [%n, %latch]
  %p6 = phi i32 [6, %entry], [%n, %latch]
  %p7 = phi i32 [7, %entry], [%n, %latch]
  %p8 = phi i32 [8, %entry], [%n, %latch]
  %p9 = phi i32 [9, %entry], [%n, %latch]
  %p10 = phi i32 [10, %entry], [%n, %latch]
  %p11 = phi i32 [11, %entry], [%n, %latch]
  call void @observe(i32 %p0)
  call void @observe(i32 %p1)
  call void @observe(i32 %p2)
  call void @observe(i32 %p3)
  call void @observe(i32 %p4)
  call void @observe(i32 %p5)
  call void @observe(i32 %p6)
  call void @observe(i32 %p7)
  call void @observe(i32 %p8)
  call void @observe(i32 %p9)
  call void @observe(i32 %p10)
  call void @observe(i32 %p11)
  switch i32 %n, label %latch [
    i32 0, label %case0
    i32 1, label %case1
    i32 2, label %case2
    i32 3, label %case3
    i32 4, label %case4
    i32 5, label %case5
    i32 6, label %case6
    i32 7, label %case7
    i32 8, label %case8
    i32 9, label %case9
  ]
case0:
  call void @observe(i32 0)
  br label %latch
case1:
  call void @observe(i32 1)
  br label %latch
case2:
  call void @observe(i32 2)
  br label %latch
case3:
  call void @observe(i32 3)
  br label %latch
case4:
  call void @observe(i32 4)
  br label %latch
case5:
  call void @observe(i32 5)
  br label %latch
case6:
  call void @observe(i32 6)
  br label %latch
case7:
  call void @observe(i32 7)
  br label %latch
case8:
  call void @observe(i32 8)
  br label %latch
case9:
  call void @observe(i32 9)
  br label %latch
latch:
  %cond = icmp ne i32 %n, 97
  br i1 %cond, label %loop, label %exit, !llvm.loop !0
exit:
  ret void
}

define void @phi_light_loop(i32 %n) {
entry:
  br label %loop
loop:
  %p0 = phi i32 [0, %entry], [%n, %latch]
  %p1 = phi i32 [1, %entry], [%n, %latch]
  %p2 = phi i32 [2, %entry], [%n, %latch]
  call void @observe(i32 %p0)
  call void @observe(i32 %p1)
  call void @observe(i32 %p2)
  switch i32 %n, label %latch [
    i32 0, label %case0
    i32 1, label %case1
    i32 2, label %case2
    i32 3, label %case3
    i32 4, label %case4
    i32 5, label %case5
    i32 6, label %case6
    i32 7, label %case7
    i32 8, label %case8
    i32 9, label %case9
  ]
case0:
  call void @observe(i32 0)
  br label %latch
case1:
  call void @observe(i32 1)
  br label %latch
case2:
  call void @observe(i32 2)
  br label %latch
case3:
  call void @observe(i32 3)
  br label %latch
case4:
  call void @observe(i32 4)
  br label %latch
case5:
  call void @observe(i32 5)
  br label %latch
case6:
  call void @observe(i32 6)
  br label %latch
case7:
  call void @observe(i32 7)
  br label %latch
case8:
  call void @observe(i32 8)
  br label %latch
case9:
  call void @observe(i32 9)
  br label %latch
latch:
  %cond = icmp ne i32 %n, 97
  br i1 %cond, label %loop, label %exit, !llvm.loop !0
exit:
  ret void
}

!0 = distinct !{!0}
