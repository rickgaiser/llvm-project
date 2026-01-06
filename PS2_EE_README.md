# PlayStation 2 Emotion Engine (R5900) LLVM Support

This document describes the extra features and instructions present in the PS2 Emotion Engine (EE) Core that are not found in standard MIPS CPUs, along with their current LLVM implementation status.

## Overview

The EE Core is based on MIPS III architecture with significant extensions:
- **128-bit GP Registers**: All 32 general-purpose registers are 128-bit wide
- **Dual Pipeline**: Two ALU pipelines (ALU0/ALU1) for parallel integer execution
- **MMI (Multimedia Instructions)**: 128-bit SIMD integer operations
- **COP1 (FPU)**: Single-precision only, with accumulator and extra operations
- **COP2 (VU0)**: 128-bit vector floating-point unit (4x32-bit floats)
- **MIPS IV Subset**: Supports MOVN, MOVZ, PREF, and FPU conditional moves
- **No LL/SC**: Load-Linked/Store-Conditional atomics are not available
- **No CLZ/CLO**: Standard CLZ/CLO not available, but PLZCW provides similar functionality
- **No DMULT/DDIV**: 64-bit multiply/divide instructions are not available (use 32-bit ops)

## Current Implementation Status

| Component | Status | Notes |
|-----------|--------|-------|
| Target Triple | **Implemented** | `mips64el-scei-ps2` auto-selects r5900 |
| Toolchain | **Implemented** | Uses `$PS2DEV` and `$PS2SDK` env vars |
| ELF Machine Flag | **Implemented** | `EF_MIPS_MACH_5900` in ELF.h |
| Linker Support | **Partial** | Recognized as MIPS III variant |
| `-mcpu=r5900` | **Implemented** | `FeatureR5900` in Mips.td |
| No LL/SC Atomics | **Implemented** | Disabled via `setMaxAtomicSizeInBitsSupported(0)` |
| No DMULT/DDIV | **Implemented** | 64-bit mul/div expanded to 32-bit ops |
| MIPS IV Subset | **Implemented** | MOVN, MOVZ, PREF, MOVN.S, MOVZ.S |
| 128-bit Registers | **Implemented** | GPR128 class with vector types (v4i32, v8i16, v16i8), LQ/SQ with patterns |
| MMI Instructions | **Partial** | Arithmetic (PADDW/H/B, PSUBW/H/B), logical (PAND/POR/PXOR/PNOR), min/max, abs, compare, GPR128 register copies (POR $d,$s,$s), bitcast patterns, **scalar i32 optimizations** (abs, min/max, saturating arithmetic, ctlz) |
| VU0 (COP2) | **Partial** | VF registers, load/store, arithmetic, ACC, VF register copies (VMOVE.xyzw) |
| Dual Pipeline | **Implemented** | 3-op MULT/MADD auto-selected; Pipeline 1 available in inline assembly |

## Compiler Flags

| Flag | Description | LLVM Status |
|------|-------------|-------------|
| `-mcpu=r5900` | Target R5900 processor | **Implemented** |
| `-mvu0` | Enable VU0 SIMD operations | Not Implemented |
| `-mabi=n32` | Use n32 ABI | **Default** for PS2 |
| `-D_EE` | Define _EE macro | **Default** for PS2 |
| `-D__ps2sdk__` | Define __ps2sdk__ macro | **Default** for PS2 |

**R5900 Scheduling**: The post-RA MachineScheduler is enabled by default for R5900, providing optimal load/multiply interleaving based on the scheduling model latencies.

## Target Triple

The recommended target triple for PS2 EE is:

```
mips64el-scei-ps2
```

This triple automatically selects `-mcpu=r5900`. Example usage:

```bash
clang --target=mips64el-scei-ps2 -c file.c
clang --target=mips64el-scei-ps2 file.c -o file.elf
```

## Building the Toolchain

A CMake cache file is provided for building a complete PS2 EE toolchain:

```bash
cmake -G Ninja -C clang/cmake/caches/PS2EE.cmake \
  -DCMAKE_INSTALL_PREFIX=$PS2DEV \
  ../llvm
ninja distribution
ninja install-distribution
```

This builds:
- Clang/LLVM targeting `mips64el-scei-ps2`
- LLD linker
- compiler-rt builtins (replaces libgcc)
- CRT files (crtbegin.o, crtend.o)

To complete the toolchain, you'll also need:
- **newlib**: C library (compile with clang)
- **crt0.o**: Startup code (from newlib or custom)

## Environment Variables

The PS2 toolchain uses environment variables to locate headers and libraries:

| Variable | Description | Example |
|----------|-------------|---------|
| `PS2DEV` | Root of PS2 development environment | `/usr/local/ps2dev` |
| `PS2SDK` | PS2SDK installation directory | `$PS2DEV/ps2sdk` |

### Expected Directory Structure

```
$PS2DEV/
  llvm/                              # LLVM toolchain installation
    bin/                             # clang, lld, etc.
    lib/clang/<version>/include/     # Clang built-in headers (stddef.h, etc.)
  mips64el-scei-ps2/                 # Target sysroot
    include/                         # newlib headers
    lib/                             # newlib libraries

$PS2SDK/
  ee/
    include/                         # PS2SDK EE-specific headers
    lib/                             # PS2SDK EE libraries
  common/
    include/                         # PS2SDK common headers
```

### Header Search Order

When compiling for `mips64el-scei-ps2`, the toolchain searches for headers in this order:

1. Clang resource directory: `$PS2DEV/llvm/lib/clang/<version>/include`
2. Explicit sysroot (if `--sysroot` specified): `<sysroot>/include`
3. PS2SDK EE headers: `$PS2SDK/ee/include`
4. PS2SDK common headers: `$PS2SDK/common/include`

Host system headers (`/usr/include`, `/usr/local/include`) are **not** included by default, ensuring a clean cross-compilation environment.

---

## R5900-Specific Registers

### General Purpose Registers (128-bit Extended)

| Register | Size | Purpose | LLVM Status |
|----------|------|---------|-------------|
| `$0-$31` | 128-bit | Extended GP registers (64-bit lower + 64-bit upper) | Not Implemented |

Standard MIPS uses 64-bit GP registers; R5900 extends these to 128-bit. Uses TImode (`__int128`) to access the full width.

### Multiply/Divide Registers (Dual Pipeline)

| Register | Size | Purpose | LLVM Status |
|----------|------|---------|-------------|
| `HI` | 64-bit | Upper result of multiply/divide (Pipeline 0) | **Implemented** (MIPS III) |
| `LO` | 64-bit | Lower result of multiply/divide (Pipeline 0) | **Implemented** (MIPS III) |
| `HI1` | 64-bit | Upper result of multiply/divide (Pipeline 1) | **Implemented** |
| `LO1` | 64-bit | Lower result of multiply/divide (Pipeline 1) | **Implemented** |

### Shift Amount Register

| Register | Size | Purpose | LLVM Status |
|----------|------|---------|-------------|
| `SA` | 8-bit | Shift amount for QFSRV (funnel shift) | **Implemented** (via MFSA/MTSA/MTSAB/MTSAH) |

### FPU (COP1) Registers

| Register | Size | Purpose | LLVM Status |
|----------|------|---------|-------------|
| `$f0-$f31` | 32-bit | FP data registers (single-precision only) | **Implemented** (MIPS III) |
| `FCR0` | 32-bit | FP Implementation/Revision (read-only) | **Implemented** (MIPS III) |
| `FCR31` | 32-bit | FP Control/Status | **Implemented** (MIPS III) |
| `ACC` | 32-bit | FP Accumulator (for ADDA.S, MULA.S, etc.) | **Implemented** |

Note: R5900 FPU is single-precision only. Double-precision is NOT supported.

### VU0/COP2 Vector Registers

| Register | Size | Purpose | LLVM Status |
|----------|------|---------|-------------|
| `$vf0-$vf31` | 128-bit | Vector FP (4x32-bit floats, xyzw) | **Implemented** |
| `$vi0-$vi15` | 16-bit | Integer registers (counters, addresses) | Not Implemented |
| `ACC` | 128-bit | Vector accumulator (4x32-bit floats) | **Implemented** |
| `Q` | 32-bit | Division/sqrt result register | Not Implemented |
| `I` | 32-bit | Immediate FP value (loaded via CTC2) | Not Implemented |

Note: `$vf0` is a constant register with value `{0.0, 0.0, 0.0, 1.0}` (w=1.0) and cannot be modified.

---

## Data Types

| Mode | Size | C Type | Register | LLVM Status |
|------|------|--------|----------|-------------|
| `QImode` | 8-bit | `char` | GP | **Implemented** |
| `HImode` | 16-bit | `short` | GP | **Implemented** |
| `SImode` | 32-bit | `int` | GP | **Implemented** |
| `SFmode` | 32-bit | `float` | FPU (COP1) | **Implemented** |
| `DImode` | 64-bit | `long long` | GP | **Implemented** |
| `TImode` | 128-bit | `__int128` | GP | Not Implemented (use vector types instead) |
| `V4SF` | 128-bit | 4 x 32-bit float | VU0 (COP2) | **Partial** (load/store/arithmetic) |
| `V16QI` | 128-bit | 16 x 8-bit int | GP (MMI) | **Implemented** (PADDB/PSUBB, PAND/POR/PXOR) |
| `V8HI` | 128-bit | 8 x 16-bit int | GP (MMI) | **Implemented** (PADDH/PSUBH, PMAXH/PMINH, PABSH, PAND/POR/PXOR) |
| `V4SI` | 128-bit | 4 x 32-bit int | GP (MMI) | **Implemented** (PADDW/PSUBW, PMAXW/PMINW, PABSW, PAND/POR/PXOR) |
| `V2DI` | 128-bit | 2 x 64-bit int | GP (MMI) | Not Implemented (no native 64-bit element SIMD) |

---

## MIPS IV Subset Instructions

The R5900 supports a subset of MIPS IV instructions. These are enabled via `FeatureMips4_r5900` in LLVM.

### Conditional Moves

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `MOVN` | Move if Not Zero | **Implemented** |
| `MOVZ` | Move if Zero | **Implemented** |
| `MOVN.S` | FP Move if Not Zero (single) | **Implemented** |
| `MOVZ.S` | FP Move if Zero (single) | **Implemented** |

### Prefetch

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `PREF` | Prefetch | **Implemented** |

Note: The R5900 does **not** support `MOVT`, `MOVF`, `MOVT.S`, `MOVF.S` (FP condition flag conditional moves) or `PREFX` (indexed prefetch).

---

## 1. 128-bit Load/Store Instructions

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `LQ` | Load Quadword (128-bit) | **Implemented** (with v4i32/v8i16/v16i8 patterns, 16-byte alignment enforced) |
| `SQ` | Store Quadword (128-bit) | **Implemented** (with v4i32/v8i16/v16i8 patterns, 16-byte alignment enforced) |

**Alignment Requirement**: LQ and SQ require 16-byte (128-bit) alignment to avoid TLB misses. The compiler automatically enforces this:
- Aligned (>= 16 bytes): Uses LQ/SQ directly
- Unaligned (< 16 bytes): Scalarizes to multiple smaller loads/stores via the stack

---

## 2. MMI (Multimedia Instructions) - 128-bit Integer SIMD

### 2.1 Arithmetic

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `PADDB` | Parallel Add Byte | **Implemented** (v16i8 add pattern) |
| `PSUBB` | Parallel Subtract Byte | **Implemented** (v16i8 sub pattern) |
| `PADDH` | Parallel Add Halfword | **Implemented** (v8i16 add pattern) |
| `PSUBH` | Parallel Subtract Halfword | **Implemented** (v8i16 sub pattern) |
| `PADDW` | Parallel Add Word | **Implemented** (v4i32 add pattern) |
| `PSUBW` | Parallel Subtract Word | **Implemented** (v4i32 sub pattern) |
| `PADDSB` | Parallel Add with Signed Saturation Byte | **Implemented** (saddsat v16i8) |
| `PSUBSB` | Parallel Subtract with Signed Saturation Byte | **Implemented** (ssubsat v16i8) |
| `PADDSH` | Parallel Add with Signed Saturation Halfword | **Implemented** (saddsat v8i16) |
| `PSUBSH` | Parallel Subtract with Signed Saturation Halfword | **Implemented** (ssubsat v8i16) |
| `PADDSW` | Parallel Add with Signed Saturation Word | **Implemented** (saddsat v4i32) |
| `PSUBSW` | Parallel Subtract with Signed Saturation Word | **Implemented** (ssubsat v4i32) |
| `PADDUB` | Parallel Add with Unsigned Saturation Byte | **Implemented** (uaddsat v16i8) |
| `PSUBUB` | Parallel Subtract with Unsigned Saturation Byte | **Implemented** (usubsat v16i8) |
| `PADDUH` | Parallel Add with Unsigned Saturation Halfword | **Implemented** (uaddsat v8i16) |
| `PSUBUH` | Parallel Subtract with Unsigned Saturation Halfword | **Implemented** (usubsat v8i16) |
| `PADDUW` | Parallel Add with Unsigned Saturation Word | **Implemented** (uaddsat v4i32) |
| `PSUBUW` | Parallel Subtract with Unsigned Saturation Word | **Implemented** (usubsat v4i32) |
| `PADSBH` | Parallel Add/Subtract Halfword | **Implemented** (asm-only) |

### 2.2 Multiply and Divide

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `PMULTW` | Parallel Multiply Word | **Implemented** (auto-selected for widening mul chains) |
| `PMULTUW` | Parallel Multiply Unsigned Word | **Implemented** (auto-selected for widening mul chains) |
| `PDIVW` | Parallel Divide Word | **Implemented** (asm-only) |
| `PDIVUW` | Parallel Divide Unsigned Word | **Implemented** (asm-only) |
| `PMADDW` | Parallel Multiply-Add Word | **Implemented** (auto-selected for widening mul chains) |
| `PMADDUW` | Parallel Multiply-Add Unsigned Word | **Implemented** (auto-selected for widening mul chains) |
| `PMSUBW` | Parallel Multiply-Subtract Word | **Implemented** (auto-selected for widening mul-sub chains) |
| `PMULTH` | Parallel Multiply Halfword | **Implemented** (asm-only) |
| `PMADDH` | Parallel Multiply-Add Halfword | **Implemented** (asm-only) |
| `PMSUBH` | Parallel Multiply-Subtract Halfword | **Implemented** (asm-only) |
| `PHMADH` | Parallel Horizontal Multiply-Add Halfword | **Implemented** (asm-only) |
| `PHMSBH` | Parallel Horizontal Multiply-Subtract Halfword | **Implemented** (asm-only) |
| `PDIVBW` | Parallel Divide Broadcast Word | **Implemented** (asm-only) |
| `PMFHI` | Parallel Move From HI Register | **Implemented** |
| `PMFLO` | Parallel Move From LO Register | **Implemented** |
| `PMTHI` | Parallel Move To HI Register | **Implemented** |
| `PMTLO` | Parallel Move To LO Register | **Implemented** |
| `PMFHL.LW` | Parallel Move From HI/LO (Low Word) | **Implemented** (used for ACC64 spill to stack) |
| `PMFHL.UW` | Parallel Move From HI/LO (Upper Word) | **Implemented** |
| `PMFHL.SLW` | Parallel Move From HI/LO (Saturating Low Word) | **Implemented** |
| `PMFHL.LH` | Parallel Move From HI/LO (Low Halfword) | **Implemented** |
| `PMFHL.SH` | Parallel Move From HI/LO (Saturating Halfword) | **Implemented** |
| `PMTHL.LW` | Parallel Move To HI/LO (Low Word) | **Implemented** (used for ACC64 restore from stack) |

### 2.3 Shift Operations

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `PSLLH` | Parallel Shift Left Logical Halfword | **Implemented** (asm-only, immediate shift) |
| `PSRLH` | Parallel Shift Right Logical Halfword | **Implemented** (asm-only, immediate shift) |
| `PSRAH` | Parallel Shift Right Arithmetic Halfword | **Implemented** (asm-only, immediate shift) |
| `PSLLW` | Parallel Shift Left Logical Word | **Implemented** (asm-only, immediate shift, all 4 words) |
| `PSLLVW` | Parallel Shift Left Logical Variable Word | **Implemented** (asm-only, **2 words only**) |
| `PSRLW` | Parallel Shift Right Logical Word | **Implemented** (asm-only, immediate shift, all 4 words) |
| `PSRLVW` | Parallel Shift Right Logical Variable Word | **Implemented** (asm-only, **2 words only**) |
| `PSRAW` | Parallel Shift Right Arithmetic Word | **Implemented** (asm-only, immediate shift, all 4 words) |
| `PSRAVW` | Parallel Shift Right Arithmetic Variable Word | **Implemented** (asm-only, **2 words only**) |

**Note on Variable Shifts (PSLLVW/PSRLVW/PSRAVW)**: These instructions only operate on **2 of 4 words** (elements 0 and 2). Elements 1 and 3 are destroyed (overwritten with sign-extended results). The v4i32 shift operations are expanded (scalarized) because no instruction shifts all 4 words with variable amounts. For immediate shifts, use PSLLW/PSRLW/PSRAW in inline assembly.

### 2.4 SA Register Operations

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `MFSA` | Move From SA Register | **Implemented** (asm-only) |
| `MTSA` | Move To SA Register | **Implemented** (asm-only) |
| `MTSAB` | Move Byte Count to SA Register | **Implemented** (asm-only) |
| `MTSAH` | Move Halfword Count to SA Register | **Implemented** (asm-only) |
| `QFSRV` | Quadword Funnel Shift Right Variable | **Implemented** (asm-only) |

### 2.5 Logical and Min/Max

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `PABSH` | Parallel Absolute Halfword | **Implemented** |
| `PABSW` | Parallel Absolute Word | **Implemented** |
| `PMAXH` | Parallel Maximum Halfword | **Implemented** |
| `PMINH` | Parallel Minimum Halfword | **Implemented** |
| `PMAXW` | Parallel Maximum Word | **Implemented** |
| `PMINW` | Parallel Minimum Word | **Implemented** |
| `PAND` | Parallel AND | **Implemented** |
| `POR` | Parallel OR | **Implemented** |
| `PXOR` | Parallel XOR | **Implemented** |
| `PNOR` | Parallel NOR | **Implemented** |

### 2.6 Compare

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `PCGTB` | Parallel Compare for Greater Than Byte | **Implemented** |
| `PCEQB` | Parallel Compare for Equal Byte | **Implemented** |
| `PCGTH` | Parallel Compare for Greater Than Halfword | **Implemented** |
| `PCEQH` | Parallel Compare for Equal Halfword | **Implemented** |
| `PCGTW` | Parallel Compare for Greater Than Word | **Implemented** |
| `PCEQW` | Parallel Compare for Equal Word | **Implemented** |
| `PLZCW` | Parallel Leading Zero Count Word | **Implemented** (also used for scalar i32 ctlz) |

### 2.7 Data Rearrangement

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `PPACB` | Parallel Pack to Byte | **Implemented** (asm-only) |
| `PPACH` | Parallel Pack to Halfword | **Implemented** (asm-only) |
| `PPACW` | Parallel Pack to Word | **Implemented** (asm-only) |
| `PPAC5` | Parallel Pack to 5 bits (RGB555 pack) | **Implemented** (asm-only) |
| `PEXTLB` | Parallel Extend Lower from Byte | **Implemented** (asm-only) |
| `PEXTLH` | Parallel Extend Lower from Halfword | **Implemented** (asm-only) |
| `PEXTLW` | Parallel Extend Lower from Word | **Implemented** (asm-only) |
| `PEXTUB` | Parallel Extend Upper from Byte | **Implemented** (asm-only) |
| `PEXTUH` | Parallel Extend Upper from Halfword | **Implemented** (asm-only) |
| `PEXTUW` | Parallel Extend Upper from Word | **Implemented** (asm-only) |
| `PEXT5` | Parallel Extend from 5 bits (RGB555 expand) | **Implemented** (asm-only) |
| `PCPYH` | Parallel Copy Halfword | **Implemented** (asm-only) |
| `PCPYLD` | Parallel Copy Lower Doubleword | **Implemented** (asm-only) |
| `PCPYUD` | Parallel Copy Upper Doubleword | **Implemented** (asm-only) |
| `PEXCH` | Parallel Exchange Center Halfword | **Implemented** (asm-only) |
| `PEXCW` | Parallel Exchange Center Word | **Implemented** (asm-only) |
| `PEXEH` | Parallel Exchange Even Halfword | **Implemented** (asm-only) |
| `PEXEW` | Parallel Exchange Even Word | **Implemented** (asm-only) |
| `PREVH` | Parallel Reverse Halfword | **Implemented** (asm-only) |
| `PINTEH` | Parallel Interleave Even Halfword | **Implemented** (asm-only) |
| `PINTH` | Parallel Interleave Halfword | **Implemented** (asm-only) |
| `PROT3W` | Parallel Rotate 3 Words | **Implemented** (asm-only) |

### 2.8 Scalar MMI Optimizations

MMI instructions can also be used for scalar i32 operations, replacing multi-instruction sequences with single instructions. Since GPR32, GPR64, and GPR128 are different views of the same physical register, the MMI instruction operates on the lower 32 bits while ignoring the upper bits.

| Scalar Operation | MMI Instruction | Replaces | LLVM Status |
|------------------|-----------------|----------|-------------|
| `abs(i32)` | `PABSW` | SRA+XOR+SUB (3 ops) | **Auto-selected** |
| `smax(i32,i32)` | `PMAXW` | SLT+MOVN (2+ ops) | **Auto-selected** |
| `smin(i32,i32)` | `PMINW` | SLT+MOVN (2+ ops) | **Auto-selected** |
| `saddsat(i32,i32)` | `PADDSW` | Multi-instruction expansion | **Auto-selected** |
| `ssubsat(i32,i32)` | `PSUBSW` | Multi-instruction expansion | **Auto-selected** |
| `uaddsat(i32,i32)` | `PADDUW` | Multi-instruction expansion | **Auto-selected** |
| `usubsat(i32,i32)` | `PSUBUW` | Multi-instruction expansion | **Auto-selected** |
| `ctlz(i32)` | `PLZCW` + cond | Software expansion (many ops) | **Auto-selected** (4 instructions) |

Note: PLZCW counts leading bits matching the sign bit (zeros for non-negative, ones for negative). The CTLZ lowering uses PLZCW+1 for non-negative values, or 0 for negative values (since negative numbers have no leading zeros).

---

## 3. Dual Pipeline Instructions

R5900 has two multiply/divide units (MAC0/Pipeline 0 and MAC1/Pipeline 1) with dedicated HI/LO registers.

### Pipeline 0 (MAC0) - Standard MIPS

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `MULT rd,rs,rt` | Multiply Word (signed) | **Implemented + Auto-selected** |
| `MULTU rd,rs,rt` | Multiply Word (unsigned) | **Implemented** |
| `DIV` | Divide Word (signed) | **Implemented** (MIPS) |
| `DIVU` | Divide Word (unsigned) | **Implemented** (MIPS) |
| `MADD rd,rs,rt` | Multiply-Add (signed) | **Implemented + Auto-selected** |
| `MADDU rd,rs,rt` | Multiply-Add (unsigned) | **Implemented + Auto-selected** |

Note: R5900 extends MULT/MULTU/MADD/MADDU to 3-operand form: `mult $rd, $rs, $rt` which writes low result to `$rd` in addition to HI:LO. The 2-operand form `mult $rs, $rt` is equivalent to 3-operand with `$zero` as destination.

### Pipeline 1 (MAC1) - R5900 Specific

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `MULT1` | Multiply Word (signed) to HI1:LO1 | **Implemented** (asm-only) |
| `MULTU1` | Multiply Word (unsigned) to HI1:LO1 | **Implemented** (asm-only) |
| `DIV1` | Divide Word (signed) to HI1:LO1 | **Implemented** (asm-only) |
| `DIVU1` | Divide Word (unsigned) to HI1:LO1 | **Implemented** (asm-only) |
| `MADD1` | Multiply-Add (signed) to HI1:LO1 | **Implemented** (asm-only) |
| `MADDU1` | Multiply-Add (unsigned) to HI1:LO1 | **Implemented** (asm-only) |
| `MFHI1` | Move From HI1 | **Implemented** (asm-only) |
| `MFLO1` | Move From LO1 | **Implemented** (asm-only) |
| `MTHI1` | Move To HI1 | **Implemented** (asm-only) |
| `MTLO1` | Move To LO1 | **Implemented** (asm-only) |

Note: Pipeline 1 instructions (MULT1, MADD1, etc.) are available in inline assembly for explicit use. The compiler uses Pipeline 0 for all generated multiply/divide operations because MULT (Slot0) can dual-issue with LW (Slot1), whereas MULT1 (Slot1) cannot. The post-RA MachineScheduler automatically interleaves loads and multiplies for optimal parallelism.

---

## 3.5 COP0 (System Control) - R5900 Specific

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `EI` | Enable Interrupts | **Implemented** |
| `DI` | Disable Interrupts | **Implemented** |

Note: R5900 has its own `EI`/`DI` instructions with different encodings than MIPS32R2. The R5900 versions are no-operand instructions (`0x42000038` for EI, `0x42000039` for DI), while MIPS32R2 uses a different format with an optional rt register.

---

## 4. COP1 (FPU) - Floating-Point Unit

R5900 FPU is single-precision only with additional operations. Double precision is NOT supported.

**Missing Standard Instructions**: The R5900 lacks `TRUNC.W.S`, `CEIL.W.S`, `FLOOR.W.S`, and `ROUND.W.S`. The `CVT.W.S` instruction always truncates toward zero (same behavior as `TRUNC.W.S` on standard MIPS). LLVM automatically uses `CVT.W.S` for float-to-int conversions on R5900.

### 4.1 Accumulator Operations

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `ADDA.S` | Add to Accumulator | **Implemented** |
| `SUBA.S` | Subtract to Accumulator | **Implemented** |
| `MULA.S` | Multiply to Accumulator | **Implemented** |
| `MADD.S` | Multiply-Add (ACC + fs * ft) | **Implemented** |
| `MADDA.S` | Multiply-Add to Accumulator | **Implemented** |
| `MSUB.S` | Multiply-Subtract (ACC - fs * ft) | **Implemented** |
| `MSUBA.S` | Multiply-Subtract to Accumulator | **Implemented** |

### 4.2 Min/Max/Reciprocal

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `MIN.S` | Floating-Point Minimum | **Implemented** |
| `MAX.S` | Floating-Point Maximum | **Implemented** |
| `RSQRT.S` | Reciprocal Square Root (fd = fs / sqrt(ft)) | **Implemented** |
| `SQRT.S` | Square Root | **Implemented** (MIPS) |

### 4.3 Compare Instructions

R5900 only supports 4 FP compare conditions. Standard MIPS compare conditions not available on R5900 are handled by LLVM through transformation:

| Instruction | Opcode | Description | LLVM Status |
|-------------|--------|-------------|-------------|
| `C.F.S` | 0x30 | False (always 0) | **Implemented** |
| `C.EQ.S` | 0x32 | Equal | **Implemented** |
| `C.LT.S` | 0x34 | Less Than (ordered) | **Implemented** |
| `C.LE.S` | 0x36 | Less Than or Equal (ordered) | **Implemented** |

**Not Supported**: C.UN, C.UEQ, C.OLT, C.ULT, C.OLE, C.ULE, and signaling variants (C.SF, C.NGLE, etc.) are not available in hardware. LLVM automatically transforms these to use supported instructions:

- **Unordered comparisons** (C.ULT, C.ULE, C.UEQ): Mapped to ordered equivalents since R5900 FPU doesn't produce NaN values
- **Greater-than comparisons**: Implemented by swapping operands (e.g., `a > b` becomes `b < a`)

### 4.4 Conditional Moves

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `MOVN.S` | Move if GPR Not Zero | **Implemented** |
| `MOVZ.S` | Move if GPR Zero | **Implemented** |
| `MOVT.S` | Move if FCC True | Not Available |
| `MOVF.S` | Move if FCC False | Not Available |

Note: R5900 lacks `MOVT.S` and `MOVF.S` (FP condition code-based moves). FP conditional moves use `MOVN.S`/`MOVZ.S` with GPR-based conditions instead.

---

## 5. COP2 (VU0) - Vector Floating-Point Unit

VU0 operates on 128-bit vectors containing 4x32-bit single-precision floats (V4SF mode).

### 5.1 Data Transfer

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `LQC2` | Load Quadword to COP2 (128-bit load to VF) | **Implemented** |
| `SQC2` | Store Quadword from COP2 (VF to 128-bit memory) | **Implemented** |
| `QMFC2` | Quadword Move From COP2 to GP | **Implemented** (GPR128 ← VF) |
| `QMTC2` | Quadword Move To COP2 from GP | **Implemented** (VF ← GPR128) |
| `CFC2` | Control Transfer from VU to EE Core | **Implemented** |
| `CTC2` | Control Transfer from EE Core to VU | **Implemented** |

### 5.2 Vector Arithmetic

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `VADD.xyzw` | dest = a + b | **Implemented** |
| `VSUB.xyzw` | dest = a - b | **Implemented** |
| `VMUL.xyzw` | dest = a * b | **Implemented** |
| `VABS.xyzw` | dest = \|a\| | **Implemented** |
| `VMAX.xyzw` | dest = max(a, b) | **Implemented** |
| `VMINI.xyzw` | dest = min(a, b) | **Implemented** |
| `VMOVE.xyzw` | dest = src | **Implemented** (masked blend ops and VF register copies) |
| `VMR32.xyzw` | dest = rotate(src) by 32 bits | **Implemented** |

### 5.3 Multiply-Accumulate

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `VMULA.xyzw` | ACC = a * b | **Implemented** |
| `VADDA.xyzw` | ACC = a + b | **Implemented** |
| `VSUBA.xyzw` | ACC = a - b | **Implemented** |
| `VMADDA.xyzw` | ACC += a * b | **Implemented** |
| `VMSUBA.xyzw` | ACC -= a * b | **Implemented** |
| `VMADD.xyzw` | dest = ACC + a * b | **Implemented** |
| `VMSUB.xyzw` | dest = ACC - a * b | **Implemented** |

### 5.4 Broadcast Operations (bc = x/y/z/w)

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `VADDbc.xyzw` | dest = a + b.bc | **Implemented** |
| `VSUBbc.xyzw` | dest = a - b.bc | **Implemented** |
| `VMULbc.xyzw` | dest = a * b.bc | **Implemented** |
| `VMADDbc.xyzw` | dest = ACC + a * b.bc | **Implemented** |
| `VMSUBbc.xyzw` | dest = ACC - a * b.bc | **Implemented** |
| `VADDAbc.xyzw` | ACC = a + b.bc | **Implemented** |
| `VSUBAbc.xyzw` | ACC = a - b.bc | **Implemented** |
| `VMULAbc.xyzw` | ACC = a * b.bc | **Implemented** |
| `VMADDAbc.xyzw` | ACC += a * b.bc | **Implemented** |
| `VMSUBAbc.xyzw` | ACC -= a * b.bc | **Implemented** |

### 5.5 Division and Square Root

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `VDIV` | Q = fs.bc / ft.bc | **Implemented** |
| `VSQRT` | Q = sqrt(ft.bc) | **Implemented** |
| `VRSQRT` | Q = fs.bc / sqrt(ft.bc) | **Implemented** |
| `VWAITQ` | Wait for Q register ready | **Implemented** |
| `VMULq.xyzw` | dest = a * Q | **Implemented** |

**Field Selection Syntax**: VDIV, VSQRT, and VRSQRT use field selector syntax where the component is appended to the VF register name:
```asm
vdiv    $Q, $vf0w, $vf1w    # Q = vf0.w / vf1.w
vrsqrt  $Q, $vf0w, $vf2w    # Q = vf0.w / sqrt(vf2.w)
vsqrt   $Q, $vf3w           # Q = sqrt(vf3.w)
```

### 5.6 Conversion Operations

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `VFTOI0.xyzw` | Float to 32-bit integer | Not Implemented |
| `VFTOI4.xyzw` | Float to 28.4 fixed-point | Not Implemented |
| `VFTOI12.xyzw` | Float to 20.12 fixed-point | Not Implemented |
| `VFTOI15.xyzw` | Float to 17.15 fixed-point | Not Implemented |
| `VITOF0.xyzw` | 32-bit integer to float | Not Implemented |
| `VITOF4.xyzw` | 28.4 fixed-point to float | Not Implemented |
| `VITOF12.xyzw` | 20.12 fixed-point to float | Not Implemented |
| `VITOF15.xyzw` | 17.15 fixed-point to float | Not Implemented |

### 5.7 Outer Product (Cross Product)

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `VOPMULA.xyz` | ACC.xyz = a.yzx * b.zxy | Not Implemented |
| `VOPMSUB.xyz` | dest.xyz = ACC - a.zxy * b.yzx | Not Implemented |

---

## Implementation Roadmap

### Phase 1: Basic R5900 Target
- [x] Add `FeatureR5900` subtarget feature in `Mips.td`
- [x] Add `r5900` processor definition
- [x] Disable LL/SC atomics for R5900
- [x] Set ELF machine flag (`EF_MIPS_MACH_5900`)
- [x] Add basic scheduling model

### Phase 2: 128-bit Support
- [x] Define 128-bit register class (GPR128) in `MipsRegisterInfo.td`
- [x] Implement LQ/SQ instructions
- [x] Register vector types (v4i32, v8i16, v16i8) with GPR128
- [ ] Add TImode (`__int128`) support (optional - vector types preferred)

### Phase 3: MMI Instructions
- [x] Register vector types in `MipsSEISelLowering.cpp`
- [x] Implement arithmetic (PADDW/H/B, PSUBW/H/B) with autovectorization patterns
- [x] Implement logical (PAND, POR, PXOR, PNOR) with patterns for all vector types
- [x] Implement min/max (PMAXW/H, PMINW/H) with smax/smin patterns
- [x] Implement absolute (PABSW, PABSH) with abs patterns
- [x] Implement compare (PCGTW/H/B, PCEQW/H/B) updated to GPR128
- [x] Add TTI hooks for autovectorization (getNumberOfRegisters, getRegisterBitWidth, getArithmeticInstrCost)
- [x] Implement data rearrangement (PPAC*, PEXT*, PCPY*, PEX*, PREV*, PINT*, PROT3W)
- [x] Implement saturating arithmetic (PADDS*, PSUBS*, PADDU*, PSUBU*) with autovectorization patterns
- [x] Implement shift operations (PSLLH/W, PSRLH/W, PSRAH/W, PSLLVW, PSRLVW, PSRAVW)
- [x] Implement halfword multiply (PMULTH, PMADDH, PMSUBH, PHMADH, PHMSBH)
- [x] Implement parallel divide (PDIVW, PDIVUW, PDIVBW)
- [x] Implement PADSBH special instruction
- [x] Add scheduling for all MMI instructions (MipsScheduleR5900.td)
- [x] Add TTI cost methods: getArithmeticReductionCost, getMinMaxReductionCost, getCastInstrCost, getCmpSelInstrCost, getInterleavedMemoryOpCost
- [ ] Add intrinsics for all MMI instructions

### Phase 4: FPU Extensions
- [x] FPU accumulator (ACC) register
- [x] ADDA.S, SUBA.S, MULA.S, MADDA.S, MSUBA.S, MADD.S, MSUB.S
- [x] MIN.S, MAX.S
- [x] RSQRT.S

### Phase 5: Dual Pipeline
- [x] HI1/LO1 registers
- [x] MULT1, MULTU1, DIV1, DIVU1
- [x] MADD1, MADDU1
- [x] MFHI1, MFLO1, MTHI1, MTLO1
- [x] 3-operand MULT/MULTU/MADD/MADDU (Pipeline 0)
- [x] Instruction scheduling for both pipelines
- [x] Pipeline 1 instructions available in inline assembly

### Phase 6: SA Register
- [x] SA register instructions (MFSA, MTSA, MTSAB, MTSAH, QFSRV)
- Note: SA register is implemented as assembly-only (no dedicated register definition needed)

### Phase 7: VU0 (COP2)
- [x] Vector register class ($vf0-$vf31)
- [x] VF0 constant register (value {0.0, 0.0, 0.0, 1.0})
- [x] LQC2/SQC2 load/store instructions
- [x] `-mvu0` compiler flag
- [x] V4SF calling convention (args in $vf12-$vf19, return in $vf1)
- [x] Vector accumulator (ACC) register
- [x] Vector arithmetic: VADD.xyzw, VSUB.xyzw, VMUL.xyzw
- [x] ACC-writing: VADDA.xyzw, VSUBA.xyzw, VMULA.xyzw
- [x] ACC multiply-accumulate: VMADD.xyzw, VMSUB.xyzw, VMADDA.xyzw, VMSUBA.xyzw
- [x] Broadcast operations: VADDbc, VSUBbc, VMULbc, VMADDbc, VMSUBbc, VADDAbc, VSUBAbc, VMULAbc, VMADDAbc, VMSUBAbc
- [x] TTI methods for autovectorization (getNumberOfRegisters, getRegisterBitWidth, getArithmeticInstrCost)
- [x] Broadcast pattern recognition: shufflevector+fmul -> VMULbc
- [x] ACC-based FMA chain optimization (VMULA/VMADD sequences via MipsR5900VU0AccChain pass)
- [x] Add scheduling for VU0 ISel variants (VADD_ISel, VMUL_ISel, etc.)
- [x] Add TTI cost methods: getArithmeticReductionCost, getMinMaxReductionCost, getCastInstrCost, getCmpSelInstrCost, getInterleavedMemoryOpCost
- [ ] Q and I register definitions
- [ ] Division and square root operations
- [ ] Conversion operations

### Phase 8: R5900 Errata
- [x] Short loop bug workaround

---

## Key Files for Implementation

### Core MIPS Files

| Purpose | File Path |
|---------|-----------|
| Feature/Processor defs | `llvm/lib/Target/Mips/Mips.td` |
| Subtarget flags | `llvm/lib/Target/Mips/MipsSubtarget.h` |
| Subtarget impl | `llvm/lib/Target/Mips/MipsSubtarget.cpp` |
| Register defs | `llvm/lib/Target/Mips/MipsRegisterInfo.td` |
| Base instructions | `llvm/lib/Target/Mips/MipsInstrInfo.td` |
| 64-bit instructions | `llvm/lib/Target/Mips/Mips64InstrInfo.td` |
| ELF flags | `llvm/include/llvm/BinaryFormat/ELF.h` |
| Linker arch tree | `lld/ELF/Arch/MipsArchTree.cpp` |

### R5900-Specific Files

| Purpose | File Path |
|---------|-----------|
| R5900 instructions | `llvm/lib/Target/Mips/MipsR5900InstrInfo.td` |
| R5900 scheduling model | `llvm/lib/Target/Mips/MipsScheduleR5900.td` |
| FPU accumulator pass | `llvm/lib/Target/Mips/MipsR5900FPUAccChain.cpp` |
| ISel patterns | `llvm/lib/Target/Mips/MipsISelDAGToDAG.cpp` |
| ISel lowering | `llvm/lib/Target/Mips/MipsISelLowering.cpp` |

### PS2 Toolchain Files

| Purpose | File Path |
|---------|-----------|
| PS2 toolchain header | `clang/lib/Driver/ToolChains/PS2.h` |
| PS2 toolchain impl | `clang/lib/Driver/ToolChains/PS2.cpp` |
| Toolchain selection | `clang/lib/Driver/Driver.cpp` |
| MIPS CPU selection | `clang/lib/Driver/ToolChains/Arch/Mips.cpp` |

---

## R5900-Specific LLVM Passes

The following custom passes optimize code generation for R5900's unique features:

| Pass | File | Stage | Description |
|------|------|-------|-------------|
| **FPU Accumulator Chain** | `MipsR5900FPUAccChain.cpp` | Pre-RA | Converts FP multiply-add sequences to use the FPU accumulator (`ACC`) with `MULA.S`/`MADDA.S` chains |

### Pass Pipeline Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                        LLVM Pass Pipeline                           │
├─────────────────────────────────────────────────────────────────────┤
│  DAG Combine                                                        │
│    └─ Widening MADD/MSUB: (sext a*b)±(sext c*d) → PMULTW+PMADDW/SUB │
│       Result is 64-bit GPR, no HI:LO extraction needed              │
├─────────────────────────────────────────────────────────────────────┤
│  Instruction Selection                                              │
│    └─ PseudoR5900MulMulAdd: (a*b)+(c*d) → MULT + MADD chain (i32)   │
├─────────────────────────────────────────────────────────────────────┤
│  Pre-Register Allocation                                            │
│    └─ MipsR5900FPUAccChain: FP mul-add → MULA.S/MADDA.S sequences   │
├─────────────────────────────────────────────────────────────────────┤
│  Register Allocation                                                │
├─────────────────────────────────────────────────────────────────────┤
│  Post-RA MachineScheduler (enabled by default)                      │
│    └─ Interleaves loads and multiplies for optimal dual-issue       │
├─────────────────────────────────────────────────────────────────────┤
│  Post-RA Pseudo Expansion                                           │
│    └─ ACC64 spill: PMFHL.LW + SD, ACC64 restore: LD + PMTHL.LW      │
├─────────────────────────────────────────────────────────────────────┤
│  Pre-Emit                                                           │
│    └─ Delay slot filler, branch expansion                           │
└─────────────────────────────────────────────────────────────────────┘
```

### Scheduling Model

The R5900 scheduling model (`MipsScheduleR5900.td`) defines:

| Resource | Pipeline | Instructions |
|----------|----------|--------------|
| `R5900MAC0` | Pipeline 0 | `MULT`, `MULTU`, `MADD`, `MADDU`, `DIV`, `DIVU` |
| `R5900MAC1` | Pipeline 1 | `MULT1`, `MULTU1`, `MADD1`, `MADDU1`, `DIV1`, `DIVU1` |
| `R5900FPUAcc` | FPU | `MULA.S`, `MADDA.S`, `MSUBA.S`, `MADD.S`, `MSUB.S` |
| `R5900UnitVU0` | VU0 | `VADD`, `VSUB`, `VMUL`, `VMADD`, `VMSUB`, `VADDA`, `VSUBA`, `VMULA`, `VMADDA`, `VMSUBA`, broadcast variants (bc) |

The dual MAC units allow two independent multiply operations to execute in parallel when properly scheduled.

#### VU0 Macro Mode Latencies

Per VU Users Manual sections 3.1.3 and 5.4.2, the VU0 ACC register has no data hazards:

| Instruction Type | Latency | Notes |
|-----------------|---------|-------|
| VF arithmetic (VADD, VSUB, VMUL) | 4 cycles | VF register result |
| ACC-init (VMULA, VADDA, VSUBA) | 1 cycle | ACC has no data hazards |
| ACC-chain (VMADDA, VMSUBA) | 1 cycle | ACC-to-ACC forwarding |
| ACC-result (VMADD, VMSUB) | 4 cycles | VF register result |
| LQC2 | 2 cycles | +1 cycle mandatory stall before next VU0 op |

The scheduler uses these latencies to interleave operations across loop iterations when unrolling, hiding the 4-cycle VF register latency by starting the next iteration's ACC chain while waiting for the previous iteration's result.

---

## References

- EE Core Instruction Set Manual (Sony)
- EE Core Users Manual (Sony)
- VU Users Manual (Sony)
- Documentation: `/home/rgaiser/dev/ps2max/docs/md/EE_Core*`
