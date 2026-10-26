; REQUIRES: asserts
; RUN: llc -mtriple=vu -pipeliner-lifetime-budget=1 -pipeliner-lifetime-budget-iis=3 \
; RUN:   -debug-only=pipeliner -o /dev/null < %S/pipeliner-pressure.ll 2>&1 \
; RUN:   | FileCheck %s

; The loop of pipeliner-pressure.ll with almost no budget for shortening the
; lifetimes: each II ends with registers beyond the limit. After three IIs in
; a row no larger II is tried.

; CHECK:      Try to schedule with 76
; CHECK:      the lifetimes are not shortened further
; CHECK:      Try to schedule with 77
; CHECK:      the lifetimes are not shortened further
; CHECK:      Try to schedule with 78
; CHECK:      the lifetimes are not shortened further
; CHECK:      Rejected the schedule because of too high register pressure
; CHECK:      No larger II: the register pressure stays beyond the limit
; CHECK-NEXT: Schedule Found? 0 (II=78)
