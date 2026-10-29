; REQUIRES: asserts
;
; A very large R5900 computed-goto function selects the bottom-up ILP-min
; MachineScheduler at -O3. Smaller functions, size-optimized functions and
; other MIPS CPUs retain their normal register-pressure-aware scheduler.
;
; RUN: %python %S/Inputs/r5900-large-dispatch.py 2048 | llc -mtriple=mips64el -mcpu=r5900 -target-abi=n32 -O3 -debug-only=mips -o /dev/null - 2>&1 | FileCheck %s --check-prefix=R5900
; RUN: %python %S/Inputs/r5900-large-dispatch.py 2045 | llc -mtriple=mips64el -mcpu=r5900 -target-abi=n32 -O3 -debug-only=mips -o /dev/null - 2>&1 | FileCheck %s --check-prefix=DEFAULT
; RUN: %python %S/Inputs/r5900-large-dispatch.py 2048 | llc -mtriple=mips64el -mcpu=r5900 -target-abi=n32 -O2 -debug-only=mips -o /dev/null - 2>&1 | FileCheck %s --check-prefix=DEFAULT
; RUN: %python %S/Inputs/r5900-large-dispatch.py 2048 optsize | llc -mtriple=mips64el -mcpu=r5900 -target-abi=n32 -O3 -debug-only=mips -o /dev/null - 2>&1 | FileCheck %s --check-prefix=DEFAULT
; RUN: %python %S/Inputs/r5900-large-dispatch.py 2048 | llc -mtriple=mips64el -mcpu=mips64r2 -target-abi=n32 -O3 -debug-only=mips -o /dev/null - 2>&1 | FileCheck %s --check-prefix=GENERIC --implicit-check-not="R5900 MachineScheduler: ILP-min"
;
; R5900: R5900 MachineScheduler: ILP-min for very_large_dispatch
; DEFAULT-NOT: R5900 MachineScheduler: ILP-min
; DEFAULT: Mips MachineScheduler: normal for very_large_dispatch
; GENERIC: Target Transform Info Pass Added
