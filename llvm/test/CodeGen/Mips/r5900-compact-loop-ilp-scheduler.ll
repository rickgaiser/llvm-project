; REQUIRES: asserts
; RUN: llc -mtriple=mips64el -mcpu=r5900 -target-abi=n32 -debug-only=mips-subtarget -o /dev/null %s 2>&1 | FileCheck %s --check-prefix=R5900
; RUN: llc -mtriple=mips64el -mcpu=mips3 -target-abi=n32 -debug-only=mips-subtarget -o /dev/null %s 2>&1 | FileCheck %s --check-prefix=MIPS3 --allow-empty
; Verify compact R5900 loops use the ILP SelectionDAG scheduler while a
; similarly sized non-loop CFG and generic MIPS preserve existing behavior.
; R5900-DAG: R5900 DAG scheduler: ILP for compact_loop
; R5900-DAG: R5900 DAG scheduler: source for ordinary_blocks
; R5900-DAG: R5900 DAG scheduler: source for light_loop
; MIPS3-NOT: R5900 DAG scheduler:
declare void @observe(i32)

define void @compact_loop(i32 %n) {
entry:
  br label %dispatch
dispatch:
  switch i32 %n, label %latch [
    i32 0, label %b0
    i32 1, label %b1
    i32 2, label %b2
    i32 3, label %b3
    i32 4, label %b4
    i32 5, label %b5
    i32 6, label %b6
    i32 7, label %b7
    i32 8, label %b8
    i32 9, label %b9
    i32 10, label %b10
    i32 11, label %b11
    i32 12, label %b12
    i32 13, label %b13
    i32 14, label %b14
    i32 15, label %b15
    i32 16, label %b16
    i32 17, label %b17
    i32 18, label %b18
    i32 19, label %b19
    i32 20, label %b20
    i32 21, label %b21
  ]
b0:
  call void @observe(i32 0)
  call void @observe(i32 0)
  call void @observe(i32 0)
  call void @observe(i32 0)
  call void @observe(i32 0)
  call void @observe(i32 0)
  call void @observe(i32 0)
  call void @observe(i32 0)
  br label %latch
b1:
  call void @observe(i32 1)
  call void @observe(i32 1)
  call void @observe(i32 1)
  call void @observe(i32 1)
  call void @observe(i32 1)
  call void @observe(i32 1)
  call void @observe(i32 1)
  call void @observe(i32 1)
  br label %latch
b2:
  call void @observe(i32 2)
  call void @observe(i32 2)
  call void @observe(i32 2)
  call void @observe(i32 2)
  call void @observe(i32 2)
  call void @observe(i32 2)
  call void @observe(i32 2)
  call void @observe(i32 2)
  br label %latch
b3:
  call void @observe(i32 3)
  call void @observe(i32 3)
  call void @observe(i32 3)
  call void @observe(i32 3)
  call void @observe(i32 3)
  call void @observe(i32 3)
  call void @observe(i32 3)
  call void @observe(i32 3)
  br label %latch
b4:
  call void @observe(i32 4)
  call void @observe(i32 4)
  call void @observe(i32 4)
  call void @observe(i32 4)
  call void @observe(i32 4)
  call void @observe(i32 4)
  call void @observe(i32 4)
  call void @observe(i32 4)
  br label %latch
b5:
  call void @observe(i32 5)
  call void @observe(i32 5)
  call void @observe(i32 5)
  call void @observe(i32 5)
  call void @observe(i32 5)
  call void @observe(i32 5)
  call void @observe(i32 5)
  call void @observe(i32 5)
  br label %latch
b6:
  call void @observe(i32 6)
  call void @observe(i32 6)
  call void @observe(i32 6)
  call void @observe(i32 6)
  call void @observe(i32 6)
  call void @observe(i32 6)
  call void @observe(i32 6)
  call void @observe(i32 6)
  br label %latch
b7:
  call void @observe(i32 7)
  call void @observe(i32 7)
  call void @observe(i32 7)
  call void @observe(i32 7)
  call void @observe(i32 7)
  call void @observe(i32 7)
  call void @observe(i32 7)
  call void @observe(i32 7)
  br label %latch
b8:
  call void @observe(i32 8)
  call void @observe(i32 8)
  call void @observe(i32 8)
  call void @observe(i32 8)
  call void @observe(i32 8)
  call void @observe(i32 8)
  call void @observe(i32 8)
  call void @observe(i32 8)
  br label %latch
b9:
  call void @observe(i32 9)
  call void @observe(i32 9)
  call void @observe(i32 9)
  call void @observe(i32 9)
  call void @observe(i32 9)
  call void @observe(i32 9)
  call void @observe(i32 9)
  call void @observe(i32 9)
  br label %latch
b10:
  call void @observe(i32 10)
  call void @observe(i32 10)
  call void @observe(i32 10)
  call void @observe(i32 10)
  call void @observe(i32 10)
  call void @observe(i32 10)
  call void @observe(i32 10)
  call void @observe(i32 10)
  br label %latch
b11:
  call void @observe(i32 11)
  call void @observe(i32 11)
  call void @observe(i32 11)
  call void @observe(i32 11)
  call void @observe(i32 11)
  call void @observe(i32 11)
  call void @observe(i32 11)
  call void @observe(i32 11)
  br label %latch
b12:
  call void @observe(i32 12)
  call void @observe(i32 12)
  call void @observe(i32 12)
  call void @observe(i32 12)
  call void @observe(i32 12)
  call void @observe(i32 12)
  call void @observe(i32 12)
  call void @observe(i32 12)
  br label %latch
b13:
  call void @observe(i32 13)
  call void @observe(i32 13)
  call void @observe(i32 13)
  call void @observe(i32 13)
  call void @observe(i32 13)
  call void @observe(i32 13)
  call void @observe(i32 13)
  call void @observe(i32 13)
  br label %latch
b14:
  call void @observe(i32 14)
  call void @observe(i32 14)
  call void @observe(i32 14)
  call void @observe(i32 14)
  call void @observe(i32 14)
  call void @observe(i32 14)
  call void @observe(i32 14)
  call void @observe(i32 14)
  br label %latch
b15:
  call void @observe(i32 15)
  call void @observe(i32 15)
  call void @observe(i32 15)
  call void @observe(i32 15)
  call void @observe(i32 15)
  call void @observe(i32 15)
  call void @observe(i32 15)
  call void @observe(i32 15)
  br label %latch
b16:
  call void @observe(i32 16)
  call void @observe(i32 16)
  call void @observe(i32 16)
  call void @observe(i32 16)
  call void @observe(i32 16)
  call void @observe(i32 16)
  call void @observe(i32 16)
  call void @observe(i32 16)
  br label %latch
b17:
  call void @observe(i32 17)
  call void @observe(i32 17)
  call void @observe(i32 17)
  call void @observe(i32 17)
  call void @observe(i32 17)
  call void @observe(i32 17)
  call void @observe(i32 17)
  call void @observe(i32 17)
  br label %latch
b18:
  call void @observe(i32 18)
  call void @observe(i32 18)
  call void @observe(i32 18)
  call void @observe(i32 18)
  call void @observe(i32 18)
  call void @observe(i32 18)
  call void @observe(i32 18)
  call void @observe(i32 18)
  br label %latch
b19:
  call void @observe(i32 19)
  call void @observe(i32 19)
  call void @observe(i32 19)
  call void @observe(i32 19)
  call void @observe(i32 19)
  call void @observe(i32 19)
  call void @observe(i32 19)
  call void @observe(i32 19)
  br label %latch
b20:
  call void @observe(i32 20)
  call void @observe(i32 20)
  call void @observe(i32 20)
  call void @observe(i32 20)
  call void @observe(i32 20)
  call void @observe(i32 20)
  call void @observe(i32 20)
  call void @observe(i32 20)
  br label %latch
b21:
  call void @observe(i32 21)
  call void @observe(i32 21)
  call void @observe(i32 21)
  call void @observe(i32 21)
  call void @observe(i32 21)
  call void @observe(i32 21)
  call void @observe(i32 21)
  call void @observe(i32 21)
  br label %latch
latch:
  %cond = icmp ne i32 %n, 42
  br i1 %cond, label %dispatch, label %exit, !llvm.loop !0
exit:
  ret void
}

define void @ordinary_blocks(i32 %n) {
entry:
  br label %dispatch
dispatch:
  switch i32 %n, label %latch [
    i32 0, label %b0
    i32 1, label %b1
    i32 2, label %b2
    i32 3, label %b3
    i32 4, label %b4
    i32 5, label %b5
    i32 6, label %b6
    i32 7, label %b7
    i32 8, label %b8
    i32 9, label %b9
    i32 10, label %b10
    i32 11, label %b11
    i32 12, label %b12
    i32 13, label %b13
    i32 14, label %b14
    i32 15, label %b15
    i32 16, label %b16
    i32 17, label %b17
    i32 18, label %b18
    i32 19, label %b19
    i32 20, label %b20
    i32 21, label %b21
  ]
b0:
  call void @observe(i32 0)
  call void @observe(i32 0)
  call void @observe(i32 0)
  call void @observe(i32 0)
  call void @observe(i32 0)
  call void @observe(i32 0)
  call void @observe(i32 0)
  call void @observe(i32 0)
  br label %latch
b1:
  call void @observe(i32 1)
  call void @observe(i32 1)
  call void @observe(i32 1)
  call void @observe(i32 1)
  call void @observe(i32 1)
  call void @observe(i32 1)
  call void @observe(i32 1)
  call void @observe(i32 1)
  br label %latch
b2:
  call void @observe(i32 2)
  call void @observe(i32 2)
  call void @observe(i32 2)
  call void @observe(i32 2)
  call void @observe(i32 2)
  call void @observe(i32 2)
  call void @observe(i32 2)
  call void @observe(i32 2)
  br label %latch
b3:
  call void @observe(i32 3)
  call void @observe(i32 3)
  call void @observe(i32 3)
  call void @observe(i32 3)
  call void @observe(i32 3)
  call void @observe(i32 3)
  call void @observe(i32 3)
  call void @observe(i32 3)
  br label %latch
b4:
  call void @observe(i32 4)
  call void @observe(i32 4)
  call void @observe(i32 4)
  call void @observe(i32 4)
  call void @observe(i32 4)
  call void @observe(i32 4)
  call void @observe(i32 4)
  call void @observe(i32 4)
  br label %latch
b5:
  call void @observe(i32 5)
  call void @observe(i32 5)
  call void @observe(i32 5)
  call void @observe(i32 5)
  call void @observe(i32 5)
  call void @observe(i32 5)
  call void @observe(i32 5)
  call void @observe(i32 5)
  br label %latch
b6:
  call void @observe(i32 6)
  call void @observe(i32 6)
  call void @observe(i32 6)
  call void @observe(i32 6)
  call void @observe(i32 6)
  call void @observe(i32 6)
  call void @observe(i32 6)
  call void @observe(i32 6)
  br label %latch
b7:
  call void @observe(i32 7)
  call void @observe(i32 7)
  call void @observe(i32 7)
  call void @observe(i32 7)
  call void @observe(i32 7)
  call void @observe(i32 7)
  call void @observe(i32 7)
  call void @observe(i32 7)
  br label %latch
b8:
  call void @observe(i32 8)
  call void @observe(i32 8)
  call void @observe(i32 8)
  call void @observe(i32 8)
  call void @observe(i32 8)
  call void @observe(i32 8)
  call void @observe(i32 8)
  call void @observe(i32 8)
  br label %latch
b9:
  call void @observe(i32 9)
  call void @observe(i32 9)
  call void @observe(i32 9)
  call void @observe(i32 9)
  call void @observe(i32 9)
  call void @observe(i32 9)
  call void @observe(i32 9)
  call void @observe(i32 9)
  br label %latch
b10:
  call void @observe(i32 10)
  call void @observe(i32 10)
  call void @observe(i32 10)
  call void @observe(i32 10)
  call void @observe(i32 10)
  call void @observe(i32 10)
  call void @observe(i32 10)
  call void @observe(i32 10)
  br label %latch
b11:
  call void @observe(i32 11)
  call void @observe(i32 11)
  call void @observe(i32 11)
  call void @observe(i32 11)
  call void @observe(i32 11)
  call void @observe(i32 11)
  call void @observe(i32 11)
  call void @observe(i32 11)
  br label %latch
b12:
  call void @observe(i32 12)
  call void @observe(i32 12)
  call void @observe(i32 12)
  call void @observe(i32 12)
  call void @observe(i32 12)
  call void @observe(i32 12)
  call void @observe(i32 12)
  call void @observe(i32 12)
  br label %latch
b13:
  call void @observe(i32 13)
  call void @observe(i32 13)
  call void @observe(i32 13)
  call void @observe(i32 13)
  call void @observe(i32 13)
  call void @observe(i32 13)
  call void @observe(i32 13)
  call void @observe(i32 13)
  br label %latch
b14:
  call void @observe(i32 14)
  call void @observe(i32 14)
  call void @observe(i32 14)
  call void @observe(i32 14)
  call void @observe(i32 14)
  call void @observe(i32 14)
  call void @observe(i32 14)
  call void @observe(i32 14)
  br label %latch
b15:
  call void @observe(i32 15)
  call void @observe(i32 15)
  call void @observe(i32 15)
  call void @observe(i32 15)
  call void @observe(i32 15)
  call void @observe(i32 15)
  call void @observe(i32 15)
  call void @observe(i32 15)
  br label %latch
b16:
  call void @observe(i32 16)
  call void @observe(i32 16)
  call void @observe(i32 16)
  call void @observe(i32 16)
  call void @observe(i32 16)
  call void @observe(i32 16)
  call void @observe(i32 16)
  call void @observe(i32 16)
  br label %latch
b17:
  call void @observe(i32 17)
  call void @observe(i32 17)
  call void @observe(i32 17)
  call void @observe(i32 17)
  call void @observe(i32 17)
  call void @observe(i32 17)
  call void @observe(i32 17)
  call void @observe(i32 17)
  br label %latch
b18:
  call void @observe(i32 18)
  call void @observe(i32 18)
  call void @observe(i32 18)
  call void @observe(i32 18)
  call void @observe(i32 18)
  call void @observe(i32 18)
  call void @observe(i32 18)
  call void @observe(i32 18)
  br label %latch
b19:
  call void @observe(i32 19)
  call void @observe(i32 19)
  call void @observe(i32 19)
  call void @observe(i32 19)
  call void @observe(i32 19)
  call void @observe(i32 19)
  call void @observe(i32 19)
  call void @observe(i32 19)
  br label %latch
b20:
  call void @observe(i32 20)
  call void @observe(i32 20)
  call void @observe(i32 20)
  call void @observe(i32 20)
  call void @observe(i32 20)
  call void @observe(i32 20)
  call void @observe(i32 20)
  call void @observe(i32 20)
  br label %latch
b21:
  call void @observe(i32 21)
  call void @observe(i32 21)
  call void @observe(i32 21)
  call void @observe(i32 21)
  call void @observe(i32 21)
  call void @observe(i32 21)
  call void @observe(i32 21)
  call void @observe(i32 21)
  br label %latch
latch:
  %cond = icmp ne i32 %n, 42
  br i1 %cond, label %dispatch, label %exit
exit:
  ret void
}

define void @light_loop(i32 %n) {
entry:
  br label %dispatch
dispatch:
  switch i32 %n, label %latch [
    i32 0, label %b0
    i32 1, label %b1
    i32 2, label %b2
    i32 3, label %b3
    i32 4, label %b4
    i32 5, label %b5
    i32 6, label %b6
    i32 7, label %b7
    i32 8, label %b8
    i32 9, label %b9
    i32 10, label %b10
    i32 11, label %b11
    i32 12, label %b12
    i32 13, label %b13
    i32 14, label %b14
    i32 15, label %b15
    i32 16, label %b16
    i32 17, label %b17
    i32 18, label %b18
    i32 19, label %b19
    i32 20, label %b20
    i32 21, label %b21
  ]
b0:
  call void @observe(i32 0)
  br label %latch
b1:
  call void @observe(i32 1)
  br label %latch
b2:
  call void @observe(i32 2)
  br label %latch
b3:
  call void @observe(i32 3)
  br label %latch
b4:
  call void @observe(i32 4)
  br label %latch
b5:
  call void @observe(i32 5)
  br label %latch
b6:
  call void @observe(i32 6)
  br label %latch
b7:
  call void @observe(i32 7)
  br label %latch
b8:
  call void @observe(i32 8)
  br label %latch
b9:
  call void @observe(i32 9)
  br label %latch
b10:
  call void @observe(i32 10)
  br label %latch
b11:
  call void @observe(i32 11)
  br label %latch
b12:
  call void @observe(i32 12)
  br label %latch
b13:
  call void @observe(i32 13)
  br label %latch
b14:
  call void @observe(i32 14)
  br label %latch
b15:
  call void @observe(i32 15)
  br label %latch
b16:
  call void @observe(i32 16)
  br label %latch
b17:
  call void @observe(i32 17)
  br label %latch
b18:
  call void @observe(i32 18)
  br label %latch
b19:
  call void @observe(i32 19)
  br label %latch
b20:
  call void @observe(i32 20)
  br label %latch
b21:
  call void @observe(i32 21)
  br label %latch
latch:
  %cond = icmp ne i32 %n, 42
  br i1 %cond, label %dispatch, label %exit, !llvm.loop !0
exit:
  ret void
}
!0 = distinct !{!0}
