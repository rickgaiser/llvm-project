# R5900 Short Loop Bug

## Overview

The R5900 processor (Toshiba TN5900, used in the PlayStation 2 Emotion Engine) has a hardware erratum that affects short loops. This document describes the bug conditions and symptoms.

## Bug Description

Under specific conditions, a loop may execute only 1-2 iterations instead of the expected number of iterations.

## Conditions That Trigger the Bug

The R5900 short loop bug occurs when **all** of the following conditions are met:

1. **Short loop**: The loop consists of **6 or fewer instructions** (including the branch instruction and its delay slot)
2. **Backward branch**: The loop contains a conditional branch that jumps backward (to an earlier address in the code)
3. **No other branches**: The loop body contains no other branch or jump instructions
4. **Non-NOP delay slot**: The branch delay slot is not a NOP (EE 2.9 or later behavior)

## Workaround

The workaround is to ensure that short loops have a NOP in their branch delay slot. This prevents the hardware erratum from triggering.

## References

- Sony EE user documentation
- r5900check tool source code comments
- PS2 development community documentation
