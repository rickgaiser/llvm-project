# PlayStation 2 Emotion Engine (R5900) LLVM Support

This document describes the extra features and instructions present in the PS2 Emotion Engine (EE) Core that are not found in standard MIPS CPUs, along with their current LLVM implementation status.

## Overview

The EE Core is based on MIPS III architecture with significant extensions:
- **128-bit GP Registers**: All 32 general-purpose registers are 128-bit wide
- **Dual Pipeline**: Two ALU pipelines (ALU0/ALU1) for parallel integer execution
- **MMI (Multimedia Instructions)**: 128-bit SIMD integer operations
- **COP1 (FPU)**: Single-precision only, with accumulator and extra operations
- **COP2 (VU0)**: 128-bit vector floating-point unit (4x32-bit floats)
- **No LL/SC**: Load-Linked/Store-Conditional atomics are not available
- **No CLZ/CLO**: Count Leading Zeros/Ones instructions are not available
- **No DMULT/DDIV**: 64-bit multiply/divide instructions are not available (use 32-bit ops)

## Current Implementation Status

| Component | Status | Notes |
|-----------|--------|-------|
| ELF Machine Flag | **Implemented** | `EF_MIPS_MACH_5900` in ELF.h |
| Linker Support | **Partial** | Recognized as MIPS III variant |
| `-mcpu=r5900` | **Implemented** | `FeatureR5900` in Mips.td |
| No LL/SC Atomics | **Implemented** | Disabled via `setMaxAtomicSizeInBitsSupported(0)` |
| No DMULT/DDIV | **Implemented** | 64-bit mul/div expanded to 32-bit ops |
| 128-bit Registers | Not Implemented | |
| MMI Instructions | Not Implemented | |
| VU0 (COP2) | **Partial** | VF registers, load/store, arithmetic, ACC implemented |
| Dual Pipeline | **Implemented** | 3-op MULT/MADD auto-selected; Pipeline 1 available in inline assembly |

## Compiler Flags

| Flag | Description | LLVM Status |
|------|-------------|-------------|
| `-mcpu=r5900` | Target R5900 processor | **Implemented** |
| `-mvu0` | Enable VU0 SIMD operations | **Implemented** |
| `-mfix-r5900` | Enable R5900 short loop erratum workaround | **Implemented** (default on) |

**R5900 Scheduling**: The post-RA MachineScheduler is enabled by default for R5900, providing optimal load/multiply interleaving based on the scheduling model latencies.

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
| `SA` | 8-bit | Shift amount for QFSRV (funnel shift) | Not Implemented |

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
| `TImode` | 128-bit | `__int128` | GP | Not Implemented |
| `V4SF` | 128-bit | 4 x 32-bit float | VU0 (COP2) | **Partial** (load/store/arithmetic) |
| `V16QI` | 128-bit | 16 x 8-bit int | GP (MMI) | Not Implemented |
| `V8HI` | 128-bit | 8 x 16-bit int | GP (MMI) | Not Implemented |
| `V4SI` | 128-bit | 4 x 32-bit int | GP (MMI) | Not Implemented |
| `V2DI` | 128-bit | 2 x 64-bit int | GP (MMI) | Not Implemented |

---

## 1. 128-bit Load/Store Instructions

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `LQ` | Load Quadword (128-bit) | Not Implemented |
| `SQ` | Store Quadword (128-bit) | Not Implemented |

---

## 2. MMI (Multimedia Instructions) - 128-bit Integer SIMD

### 2.1 Arithmetic

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `PADDB` | Parallel Add Byte | Not Implemented |
| `PSUBB` | Parallel Subtract Byte | Not Implemented |
| `PADDH` | Parallel Add Halfword | Not Implemented |
| `PSUBH` | Parallel Subtract Halfword | Not Implemented |
| `PADDW` | Parallel Add Word | Not Implemented |
| `PSUBW` | Parallel Subtract Word | Not Implemented |
| `PADDSB` | Parallel Add with Signed Saturation Byte | Not Implemented |
| `PSUBSB` | Parallel Subtract with Signed Saturation Byte | Not Implemented |
| `PADDSH` | Parallel Add with Signed Saturation Halfword | Not Implemented |
| `PSUBSH` | Parallel Subtract with Signed Saturation Halfword | Not Implemented |
| `PADDSW` | Parallel Add with Signed Saturation Word | Not Implemented |
| `PSUBSW` | Parallel Subtract with Signed Saturation Word | Not Implemented |
| `PADDUB` | Parallel Add with Unsigned Saturation Byte | Not Implemented |
| `PSUBUB` | Parallel Subtract with Unsigned Saturation Byte | Not Implemented |
| `PADDUH` | Parallel Add with Unsigned Saturation Halfword | Not Implemented |
| `PSUBUH` | Parallel Subtract with Unsigned Saturation Halfword | Not Implemented |
| `PADDUW` | Parallel Add with Unsigned Saturation Word | Not Implemented |
| `PSUBUW` | Parallel Subtract with Unsigned Saturation Word | Not Implemented |
| `PADSBH` | Parallel Add/Subtract Halfword | Not Implemented |

### 2.2 Multiply and Divide

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `PMULTW` | Parallel Multiply Word | Not Implemented |
| `PMULTUW` | Parallel Multiply Unsigned Word | Not Implemented |
| `PDIVW` | Parallel Divide Word | Not Implemented |
| `PDIVUW` | Parallel Divide Unsigned Word | Not Implemented |
| `PMADDW` | Parallel Multiply-Add Word | Not Implemented |
| `PMADDUW` | Parallel Multiply-Add Unsigned Word | Not Implemented |
| `PMSUBW` | Parallel Multiply-Subtract Word | Not Implemented |
| `PMULTH` | Parallel Multiply Halfword | Not Implemented |
| `PMADDH` | Parallel Multiply-Add Halfword | Not Implemented |
| `PMSUBH` | Parallel Multiply-Subtract Halfword | Not Implemented |
| `PHMADH` | Parallel Horizontal Multiply-Add Halfword | Not Implemented |
| `PHMSBH` | Parallel Horizontal Multiply-Subtract Halfword | Not Implemented |
| `PDIVBW` | Parallel Divide Broadcast Word | Not Implemented |
| `PMFHI` | Parallel Move From HI Register | Not Implemented |
| `PMFLO` | Parallel Move From LO Register | Not Implemented |
| `PMTHI` | Parallel Move To HI Register | Not Implemented |
| `PMTLO` | Parallel Move To LO Register | Not Implemented |
| `PMFHL.LW` | Parallel Move From HI/LO (Low Word) | Not Implemented |
| `PMFHL.UW` | Parallel Move From HI/LO (Upper Word) | Not Implemented |
| `PMFHL.SLW` | Parallel Move From HI/LO (Signed Low Word) | Not Implemented |
| `PMFHL.LH` | Parallel Move From HI/LO (Low Halfword) | Not Implemented |
| `PMFHL.SH` | Parallel Move From HI/LO (Signed Halfword) | Not Implemented |
| `PMTHL.LW` | Parallel Move To HI/LO (Low Word) | Not Implemented |

### 2.3 Shift Operations

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `PSLLH` | Parallel Shift Left Logical Halfword | Not Implemented |
| `PSRLH` | Parallel Shift Right Logical Halfword | Not Implemented |
| `PSRAH` | Parallel Shift Right Arithmetic Halfword | Not Implemented |
| `PSLLW` | Parallel Shift Left Logical Word | Not Implemented |
| `PSLLVW` | Parallel Shift Left Logical Variable Word | Not Implemented |
| `PSRLW` | Parallel Shift Right Logical Word | Not Implemented |
| `PSRLVW` | Parallel Shift Right Logical Variable Word | Not Implemented |
| `PSRAW` | Parallel Shift Right Arithmetic Word | Not Implemented |
| `PSRAVW` | Parallel Shift Right Arithmetic Variable Word | Not Implemented |

### 2.4 SA Register Operations

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `MFSA` | Move From SA Register | Not Implemented |
| `MTSA` | Move To SA Register | Not Implemented |
| `MTSAB` | Move Byte Count to SA Register | Not Implemented |
| `MTSAH` | Move Halfword Count to SA Register | Not Implemented |
| `QFSRV` | Quadword Funnel Shift Right Variable | Not Implemented |

### 2.5 Logical and Min/Max

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `PABSH` | Parallel Absolute Halfword | Not Implemented |
| `PABSW` | Parallel Absolute Word | Not Implemented |
| `PMAXH` | Parallel Maximum Halfword | Not Implemented |
| `PMINH` | Parallel Minimum Halfword | Not Implemented |
| `PMAXW` | Parallel Maximum Word | Not Implemented |
| `PMINW` | Parallel Minimum Word | Not Implemented |
| `PAND` | Parallel AND | Not Implemented |
| `POR` | Parallel OR | Not Implemented |
| `PXOR` | Parallel XOR | Not Implemented |
| `PNOR` | Parallel NOR | Not Implemented |

### 2.6 Compare

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `PCGTB` | Parallel Compare for Greater Than Byte | Not Implemented |
| `PCEQB` | Parallel Compare for Equal Byte | Not Implemented |
| `PCGTH` | Parallel Compare for Greater Than Halfword | Not Implemented |
| `PCEQH` | Parallel Compare for Equal Halfword | Not Implemented |
| `PCGTW` | Parallel Compare for Greater Than Word | Not Implemented |
| `PCEQW` | Parallel Compare for Equal Word | Not Implemented |
| `PLZCW` | Parallel Leading Zero Count Word | Not Implemented |

### 2.7 Data Rearrangement

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `PPACB` | Parallel Pack to Byte | Not Implemented |
| `PPACH` | Parallel Pack to Halfword | Not Implemented |
| `PPACW` | Parallel Pack to Word | Not Implemented |
| `PPAC5` | Parallel Pack to 5 bits (RGB555 pack) | Not Implemented |
| `PEXTLB` | Parallel Extend Lower from Byte | Not Implemented |
| `PEXTLH` | Parallel Extend Lower from Halfword | Not Implemented |
| `PEXTLW` | Parallel Extend Lower from Word | Not Implemented |
| `PEXTUB` | Parallel Extend Upper from Byte | Not Implemented |
| `PEXTUH` | Parallel Extend Upper from Halfword | Not Implemented |
| `PEXTUW` | Parallel Extend Upper from Word | Not Implemented |
| `PEXT5` | Parallel Extend from 5 bits (RGB555 expand) | Not Implemented |
| `PCPYH` | Parallel Copy Halfword | Not Implemented |
| `PCPYLD` | Parallel Copy Lower Doubleword | Not Implemented |
| `PCPYUD` | Parallel Copy Upper Doubleword | Not Implemented |
| `PEXCH` | Parallel Exchange Center Halfword | Not Implemented |
| `PEXCW` | Parallel Exchange Center Word | Not Implemented |
| `PEXEH` | Parallel Exchange Even Halfword | Not Implemented |
| `PEXEW` | Parallel Exchange Even Word | Not Implemented |
| `PREVH` | Parallel Reverse Halfword | Not Implemented |
| `PINTEH` | Parallel Interleave Even Halfword | Not Implemented |
| `PINTH` | Parallel Interleave Halfword | Not Implemented |
| `PROT3W` | Parallel Rotate 3 Words | Not Implemented |

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
| `MADDU rd,rs,rt` | Multiply-Add (unsigned) | **Implemented** |

Note: R5900 extends MULT/MULTU/MADD/MADDU to 3-operand form: `mult $rd, $rs, $rt` which writes low result to `$rd` in addition to HI:LO. The compiler automatically:
- Uses 3-operand MULT for i32 multiplies (saves `mflo` instruction)
- Uses MULT + MADD chain for `(a*b) + (c*d)` patterns (saves one instruction)

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

---

## 5. COP2 (VU0) - Vector Floating-Point Unit

VU0 operates on 128-bit vectors containing 4x32-bit single-precision floats (V4SF mode).

### 5.1 Data Transfer

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `LQC2` | Load Quadword to COP2 (128-bit load to VF) | **Implemented** |
| `SQC2` | Store Quadword from COP2 (VF to 128-bit memory) | **Implemented** |
| `QMFC2` | Quadword Move From COP2 to GP | Not Implemented (requires 128-bit GP) |
| `QMTC2` | Quadword Move To COP2 from GP | Not Implemented (requires 128-bit GP) |
| `CFC2` | Control Transfer from VU to EE Core | Not Implemented |
| `CTC2` | Control Transfer from EE Core to VU | Not Implemented |

### 5.2 Vector Arithmetic

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `VADD.xyzw` | dest = a + b | **Implemented** |
| `VSUB.xyzw` | dest = a - b | **Implemented** |
| `VMUL.xyzw` | dest = a * b | **Implemented** |
| `VABS.xyzw` | dest = \|a\| | Not Implemented |
| `VMAX.xyzw` | dest = max(a, b) | Not Implemented |
| `VMINI.xyzw` | dest = min(a, b) | Not Implemented |
| `VMOVE.xyzw` | dest = src | Not Implemented |

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
| `VADDbc.xyzw` | dest = a + b.bc | Not Implemented |
| `VSUBbc.xyzw` | dest = a - b.bc | Not Implemented |
| `VMULbc.xyzw` | dest = a * b.bc | Not Implemented |
| `VMADDbc.xyzw` | dest = ACC + a * b.bc | Not Implemented |
| `VMSUBbc.xyzw` | dest = ACC - a * b.bc | Not Implemented |

### 5.5 Division and Square Root

| Instruction | Description | LLVM Status |
|-------------|-------------|-------------|
| `VDIV` | Q = fs.bc / ft.bc | Not Implemented |
| `VSQRT` | Q = sqrt(ft.bc) | Not Implemented |
| `VRSQRT` | Q = fs.bc / sqrt(ft.bc) | Not Implemented |
| `VWAITQ` | Wait for Q register ready | Not Implemented |
| `VMULq.xyzw` | dest = a * Q | Not Implemented |

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
- [ ] Define 128-bit register class (GPR128) in `MipsRegisterInfo.td`
- [ ] Implement LQ/SQ instructions
- [ ] Add TImode (`__int128`) support

### Phase 3: MMI Instructions
- [ ] Create `MipsR5900MMIInstrInfo.td`
- [ ] Implement arithmetic (PADDW, PSUBW, etc.)
- [ ] Implement logical (PAND, POR, PXOR, PNOR)
- [ ] Implement compare (PCEQ*, PCGT*)
- [ ] Implement data rearrangement (PPAC*, PEXT*, etc.)
- [ ] Add intrinsics for all MMI instructions
- [ ] Add autovectorization patterns

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
- [ ] SA register definition
- [ ] MFSA, MTSA, MTSAB, MTSAH
- [ ] QFSRV (quadword funnel shift)

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
- [ ] Q and I register definitions
- [ ] Division and square root operations
- [ ] Conversion operations
- [ ] Broadcast operations (VADDbc, VSUBbc, VMULbc, etc.)

### Phase 8: R5900 Errata
- [x] Short loop bug workaround (`-mfix-r5900`)

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
│  Instruction Selection                                              │
│    └─ PseudoR5900MulMulAdd: (a*b)+(c*d) → MULT + MADD chain         │
├─────────────────────────────────────────────────────────────────────┤
│  Pre-Register Allocation                                            │
│    └─ MipsR5900FPUAccChain: FP mul-add → MULA.S/MADDA.S sequences   │
├─────────────────────────────────────────────────────────────────────┤
│  Register Allocation                                                │
├─────────────────────────────────────────────────────────────────────┤
│  Post-RA MachineScheduler (enabled by default)                      │
│    └─ Interleaves loads and multiplies for optimal dual-issue       │
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
| `R5900UnitVU0` | VU0 | `VADD`, `VSUB`, `VMUL`, `VMADD`, `VMSUB`, `VADDA`, `VSUBA`, `VMULA`, `VMADDA`, `VMSUBA` |

The dual MAC units allow two independent multiply operations to execute in parallel when properly scheduled.

---

## References

- EE Core Instruction Set Manual (Sony)
- EE Core Users Manual (Sony)
- VU Users Manual (Sony)
- Documentation: `/home/rgaiser/dev/ps2max/docs/md/EE_Core*`
