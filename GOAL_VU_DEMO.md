# Goal: unify the VU work and build an FPU + VU0 + VU1 demo

## Status (2026-10-23): all steps done

| Step | Result |
|---|---|
| 1. Build `wt/vu`, lit | Built. VU + Mips lit: 27 failures, all pre-existing on `r5900-feature` (MSA tests, some R5900 tests); none from `vu-target`. Now 26: `r5900-fpu-acc-chain.ll` fixed. |
| 2. One tree | `r5900-feature` fast-forwarded to `vu-target` (`b8de78a06737`). `configure.sh` (feature, feature-assert) and `build-llvm-full.sh` build the VU target; `vu` role removed from `configure.sh` and `../CLAUDE.md`. Installed: `ninja distribution install-distribution` in `toolchain/build/llvm` + the script's symlinks; `install/llvm/bin/llc` has Mips and `vu`. |
| 1.4 Comparison | Done, table in Part 1. |
| 3. `vuk` | `bench/vuk/`: `vuk.py`, `vuk.md`, `poly.vuk`, `poly4.vuk`, `test.sh` (all checks pass). Confirmed by the user. |
| 4. Demo | `bench/vudemo/` -> `toolchain/build/vudemo/vudemo_{macro,micro,all,bench,debug}.elf` and `mulprobe.elf`. All pass on PCSX2 and on the PS2 (within 4 ULP of the exact results). |
| 5. Efficiency + codegen | Commits on r5900-feature: `b882a9bbe04c`, `db7fd7b1c612` (VU pipeliner), `db3997e348e9` (R5900 FPU ACC chains with a plain term), `5e2912c1d427` (miscompile); bench `5ef8053`, `6e76974`, `ce00873`, `5f32ce4`; kernel formulation `poly4`; EE kernels built with `-fno-associative-math`. |
| 6. Hardware | Measured on the PS2 (below). |

### Final hardware results (PS2, `vudemo_bench.elf`, EE cycles per quadword, D = 16)

| Configuration | First run | Final | Result |
|---|---|---|---|
| EE FPU | 174.1 | **122.1** | within 1 ULP |
| VU0 macro | 36.6 | 37.9 | within 1 ULP |
| VU0 micro (incl. COP2 transfers) | 49.1 | 50.2 | exact |
| VU1 micro (incl. DMA and readback) | 103.2 | **61.8** | exact |
| **FPU + VU0 micro + VU1 micro** | 58.9 | **36.2** | within 1 ULP |

All three together are now the fastest configuration (1.4x VU0 micro alone, 1.7x VU1 alone, 3.4x the FPU).

### How close to the peak

- **Compute peak** (the units' kernels only, transfers free): VU0 and VU1 25 cycles/quadword each (23 FMAC
  per quadword, 16 useful), FPU 112 (static) -> together 1 / (1/25 + 1/25 + 1/112) = **11.7 cycles/quadword**.
- **What the EE must do per quadword** (measured): VU0 17 to write and 8.5 to read (COP2), VU1 26 to read
  back (uncached; nothing else moves VU1 memory to main memory) plus about 2400 per batch to send, FPU 122.
  The EE is busy for 81k of the 92k cycles of the combined run: the combined run is EE-bound, not VU-bound.
- **Bound for this design**: every quadword costs the EE at least ~25 cycles on its way through a VU, so
  about **25-28 cycles/quadword** is the best a schedule can do here; the demo reaches 36.2.
- What would lower the bound: VIF0 DMA for VU0's input (saves the 17; did not start on the PS2, see
  below), double-buffered VU1 batches (needs a base address in the vuk layout), results returned as a
  reduction instead of every quadword.

### Code generation findings and what was done

| Finding | Done |
|---|---|
| Plain Horner needs 2 FMAC per degree on a VU (MADD adds ACC, not a register): at most 50% | `poly4.vuk`: 4 degrees per step, one ACC chain of 5, 23 FMAC/quadword instead of 32 (64%). An ISA limit, not a compiler one. |
| VU loops above MII 27 were not pipelined; the pipeliner then ran out of VF registers | Fixed (`b882a9bbe04c`, `db7fd7b1c612`): plain Horner 85 -> 37 cycles/quadword. |
| R5900 FPU used mul.s + add.s for `c0 + c1 x + c2 x^2 + c3 x^3 + y x^4` | Fixed (`db3997e348e9`): the FPU ACC chain pass takes a plain term (MULA.S c0, 1.0) and puts the late product last. FPU 175 -> 122 cycles/quadword on the PS2. |
| Scalar abs/saturating MMI left a non-sign-extended upper word | Fixed (`5e2912c1d427`). |
| `-ffast-math` reassociation turns the 4-degree steps into 2-degree ones (twice the dependent chain) | The demo's EE kernels use `-fno-associative-math`. A compiler fix (do not reassociate a sum of products into a longer chain) is left open. |
| VU0 macro: 28 static, 36.6 on the PS2 | The gap is data cache misses (one line per 4 quadwords), not scheduling: ACC chains are already ordered so the y term comes last, and unrolling 4/8 does not help. Prefetch is the separate PREF topic. |

### Hardware findings

- **The R5900 multiplier is not commutative** (bench `vudemo/mulprobe.c`): with a power of two as the
  first operand (fs) and low mantissa bits in the second, `mul.s`, `vmul` and `vmula` give a product
  1 ULP low; with the power of two second, exact. PCSX2 is exact in both orders. This is why FPU and
  VU0 macro were 1 ULP off and VU0/VU1 micro exact. Possible compiler work: put the operand with the
  fewest mantissa bits (e.g. a power-of-two constant) in ft (open).
- **VU control**: writing VU memory through the memory map and starting with CMSAR1/VCALLMS gave wrong
  VU results on hardware (worked on PCSX2 and with delays). Now as in ps2gl/ps2stuff: VU1 through VIF1
  DMA (REF tags from the data, UNPACK, MSCAL, **FLUSHE** - FLUSH also waits for GIF and stalls), VU0 data
  through LQC2/VSQI and VLQI/SQC2, CFC2.I to wait.
- **DMA needs physical addresses**: passing the uncached alias (0x2...) of the tags to the DMA crashed
  the PS2; PCSX2 masks it.
- **VIF0 DMA works on the PS2** (`vudemo/vif0probe.c`: normal and chain mode, through ps2sdk and
  direct, after every step of the demo's setup). It failed in the demo only when its tags and parameters
  were written through the uncached alias; written cached + SyncDCache it works (`VU0_DMA 1`). It is
  slower here than COP2 transfers (combined 44.4 vs 36.2 cycles/quadword: SyncDCache and a DMA start
  per batch), so the demo uses COP2 for VU0 (`VU0_DMA 0`). Why the uncached tags stop channel 0 but
  not channel 1 is not understood.
- **Cache**: per-batch SyncDCache cost ~8k cycles per batch; instead x is written back once per run, the
  tags/parameters are written uncached and the results stored uncached accelerated.
- **Miscompile, fixed** (`5e2912c1d427`): `abs(-1) < 5` was false (PABSW upper word).

### Stopped here (user: "finish your work, stop improving")

The combined run is at 36.2 cycles/quadword, the fastest configuration, against a compute peak of 11.7
and a bound of about 25-28 for this design (EE-bound). Not done, as future work:
- double-buffered VU1 batches (base address in the vuk layout); a cheaper VIF0 path for VU0;
- compiler: reassociation into longer chains; the multiply operand order on the R5900;
- measure the VU micro-mode latencies (the VU target's model uses the manual's).
- Uncommitted: bench `vudemo` (VU0_DMA switch, chain_end/chain_start, vif0probe.c, debug probes).

Earlier status: `vu-target` was rebased onto `r5900-feature` in `wt/vu` (40 commits applied,
1 skipped as already present, no conflicts).

## Goal

1. One tree with both VU sides: the `VU` target (micro mode, from `vu-target`) and the VU0 macro-mode
   lowering in the Mips target (`r5900-feature`). Where they solve the same problem, keep the better one.
2. A simple demo where the EE FPU, VU0 (micro) and VU1 (micro) compute **at the same time**, and an
   estimate of how close it gets to the theoretical peak of the three units.
3. Two VU0 tests: VU0 in macro mode (COP2 code from clang `-mcpu=r5900`) and VU0 in micro mode
   (`vu` target program).
4. A small language for VU micro programs (the VU target has no frontend).

## Rules

- Correctness runs go through the **ps2-pcsx2-test** skill. The program ends with
  `printf("@@PS2TEST_END@@ status=%d\n", status)` and must not return from `main`.
- PCSX2 timings are not performance data. Performance claims come from (a) static numbers from `llc`
  (see "Peak") and (b) real-PS2 runs later, each after the user confirms "Is the PS2 ready?".
- Hardware costs are measured on the PS2 before they are trusted.
- Commits only with the user's approval, no co-author line, `git clang-format` before committing.

## Key facts

| Fact | Value | Source |
|---|---|---|
| VU0 memory | 4 KB micro, 4 KB data (256 quads) | VU Users Manual, memory map |
| VU1 memory | 16 KB micro, 16 KB data (1024 quads) | VU Users Manual, memory map |
| Start VU0 micro | `vcallms` from the EE | VU Users Manual |
| Start VU1 micro | write CMSAR1 with `ctc2` (Stop -> Run; ignored while VU1 runs) | VU Users Manual |
| Wait for a VU | poll VPU-STAT (`cfc2`) | VU Users Manual |
| FPU vs VU0 macro | same EE instruction stream; `vadd` does not pair with `mul.s` | measured (bench uarch) |
| FP semantics | R5900 FPU and VUs are not IEEE (no denormals/inf/NaN, own rounding) | manuals |

Consequence: FPU and VU0 macro mode cannot run in parallel. The parallel setup is
**EE FPU + VU0 micro + VU1 micro**. VU0 macro mode is a comparison point, not a fourth worker.

## Part 1: one tree

1. Build `wt/vu` in `toolchain/build/llvm-vu` (`configure.sh vu` already builds Mips + VU):
   `ninja -C toolchain/build/llvm-vu llc llvm-mc FileCheck not count`.
   Run lit on `CodeGen/VU`, `MC/VU`, `MC/Disassembler/VU`, `CodeGen/Mips`, `MC/Mips`.
2. When it passes: fast-forward `r5900-feature` to `vu-target`. Add
   `-DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=VU` to the `feature` configuration in `configure.sh`
   and `build-llvm-full.sh`, so the installed toolchain has clang (EE) and `llc -mtriple=vu` together.
3. Retire the separate `vu` role. Update the worktree table in `../CLAUDE.md`
   (the `vu` row still says "based on origin/main, no clang").
4. Compare the two VU sides, limited to what the demo needs. **Done (2026-10-23)**, from the code of both
   sides and the generated code of the demo kernels (`poly4` in `vu0_poly.s` / `vu1_poly.s` and in `macro.s`):

| Topic | VU micro (`VU` target) | VU0 macro (Mips target) | Result: what each side keeps or takes |
|---|---|---|---|
| Sums of products | ISel to MULA/MADDA/MADD, a plain term as `c * vf0.w`, field sums with ADDA and a vector of ones | ISel ACC chains (`80c4c5ecf864`), a plain term as `vmulaw c, vf0` | The same chain on both: 5 FMAC per 4-degree step, 23 per quadword. **Taken over to the FPU**: the R5900 FPU ACC chain pass now also takes one plain term (as `mula.s c, 1.0`) and orders the products so the one that waits for the previous step is the final `madd.s` (FPU 41.5 -> 28 cycles/float static, 175 -> 122 cycles/quadword on the PS2). |
| Operand order of products | `madd y, y, v4` (the power of two in ft) | `vmadd y, v4, y` (in fs) | Measured: the multiplier is 1 ULP low with a power of two in fs (mulprobe). Micro results exact, macro/FPU 1 ULP off. Neither side chooses the order on purpose yet (open; only matters for accuracy). |
| Scheduling model | Itineraries, FDIV/EFU as resources, latencies from the manual, `CompleteModel = 0` | Measured (`MipsScheduleR5900.td`): FMAC 4, ACC->ACC 1, Q after VDIV 9 / VRSQRT 15, C2 slot, pipe 0/1 | Kept separate: macro mode includes the EE issue rules, micro mode is the VU's own pipeline. Micro latencies are not measured yet (open; the loop schedules match the manual: 25 words per quadword, 23 FMAC + 2 bubbles). |
| Loops | Software pipelining (iterative modulo scheduling, policy: 8 stages, now no MII limit) | No pipeliner; partial unrolling + the R5900 dual-issue pass | Both reach the resource bound of one quadword (VU: II = ResMII 23 -> 25 words; macro: 28 static). The macro loop is 36.6 on the PS2: the difference is data cache misses (one line per 4 quadwords), not scheduling, so a pipeliner would not help it; prefetch (separate PREF topic) would. |
| Single-register values (ACC, Q) | `ResultRegChains` mutation: keeps ACC/Q groups together for the pipeliner | Accumulator DAG mutation (`MipsDAGMutations.cpp`) for HI/LO, FPU ACC, VU0 ACC | Same problem, different scheduler (pipeliner vs MachineScheduler). Kept both. |
| Register pressure | VF pressure set included ACC and Q; fixed (`db7fd7b1c612`) | n/a (VF registers are not pressure-limited in macro code) | VU only. |
| Memory | `llvm.vu.lq/sq` (quadword addresses in VI registers) | `lqc2`/`sqc2` with GPR addresses | Different address spaces; nothing to share. |
| Delay slots, hazards, issue | Packetizer (upper/lower words), delay-slot filler, hazard pass | R5900 aligned-block issue model and pass | Different machines; nothing to share. |

   Deferred (not needed for the demo): divide/WAITQ, VU1-only instructions.

## Part 2: the VU language, `vuk`

A tiny kernel language, compiled to LLVM IR by a short script, using `<4 x float>` and the
`llvm.vu.*` intrinsics. No clang target work needed.

- One vector type `f32x4`, scalars `f32` and `i16`, quad arrays `f32x4[]` (indices are quads),
  one loop form `for i in 0..n`, and `const` arrays for coefficients.
- Arrays are offsets in VU data memory (no pointers), which matches `llvm.vu.lq/sq`.
- Out of scope: reductions, branches inside loops, functions calling functions.

Location: `bench/vuk/` (script + `vuk.md` grammar + a few `llc -mtriple=vu` lit-style checks),
not in the LLVM tree.

Rejected: hand-written LLVM IR (too verbose), a C subset through clang (too large for this demo).

## Part 3: the demo

Workload: **polynomial evaluation by Horner's method**, `y[i] = p(x[i])` with degree `D`.
One load and one store per quad but `D` multiply-adds, so it is compute-bound and `D` sets how much.
Data stays in VU data memory during the computation (no DMA).

Exact-result inputs, so the checksum can be compared bit for bit with a host reference:
`x[i]` in {-1, -0.5, 0, 0.5, 1}, integer coefficients in -4..4, `D <= 16`.
Every intermediate fits in 24 mantissa bits, so rounding differences cannot show.

Split (sizes follow from VU memory; x and y in place, so 1 quad per element plus coefficients):

| Unit | Elements (quads) | Code |
|---|---|---|
| EE FPU | `N_FPU` | C, scalar `float` loop |
| VU0 micro | up to ~240 | `poly.vuk` with `llc -mcpu=vu0` |
| VU1 micro | up to ~1000 | `poly.vuk` with `llc -mcpu=vu1` |

Choose `N_FPU` so all three finish at about the same time, from the static cycle counts.

### `poly.vuk`

```
kernel poly(x: f32x4[], n: i16, c: const f32[D + 1]) {
    for i in 0..n {
        var y = c[D];
        for k in D - 1 ..= 0 {        // unrolled: D is a compile-time constant
            y = y * x[i] + c[k];
        }
        x[i] = y;                     // in place
    }
}
```

Build: `vuk poly.vuk -D D=16 -o poly.ll`, then `llc -mtriple=vu -mcpu=vu1 -filetype=obj`
(and `-mcpu=vu0`). Extract the code with `llvm-objcopy -O binary` and include it in the EE
program with `.incbin`.

### `demo.c` (clang `-mtriple=mips64el -mcpu=r5900`, built like `apps/test_llvm_01`)

```c
#define VU0_DATA ((volatile f32x4 *)0x11004000)
#define VU1_DATA ((volatile f32x4 *)0x1100C000)

extern const char vu0_poly[], vu0_poly_end[], vu1_poly[], vu1_poly_end[];   // .incbin

static float poly_fpu(float x) {                   // FPU part, same polynomial
    float y = C[D];
    for (int k = D - 1; k >= 0; k--) y = y * x + C[k];
    return y;
}

static f32x4 poly_vu0_macro(f32x4 x) {             // test A: VU0 macro mode, COP2 from clang
    f32x4 y = (f32x4){C[D], C[D], C[D], C[D]};
    for (int k = D - 1; k >= 0; k--) y = y * x + C[k];
    return y;
}

int main(void) {
    init_inputs();
    load_micro(VU0, vu0_poly, vu0_poly_end);       // copy into micro memory while the VU is stopped
    load_micro(VU1, vu1_poly, vu1_poly_end);
    copy_in(VU0_DATA, part_vu0, N_VU0);
    copy_in(VU1_DATA, part_vu1, N_VU1);

    vu1_start(0);                                  // ctc2 CMSAR1
    vu0_start(0);                                  // vcallms
    for (int i = 0; i < N_FPU; i++)                // FPU works while both VUs run
        part_fpu[i] = poly_fpu(part_fpu[i]);
    vu_wait();                                     // poll VPU-STAT

    copy_out(part_vu0, VU0_DATA, N_VU0);
    copy_out(part_vu1, VU1_DATA, N_VU1);
    unsigned sum = checksum();
    printf("Result: %08x\n", sum);
    printf("@@PS2TEST_END@@ status=%d\n", sum != EXPECTED);
    for (;;);                                      // never return (PCSX2 skill)
}
```

Tests (separate ELFs or one ELF with a switch; either is fine):

1. `test_vu0_macro`: VU0 part with `poly_vu0_macro` (no VU1, no VU0 micro).
2. `test_vu0_micro`: VU0 micro only.
3. `demo_all`: FPU + VU0 micro + VU1 micro in parallel.

Each prints `Result:` and the end marker, and must match the host checksum on PCSX2.

### Peak

Per unit: peak flops/cycle = lanes x 2 (multiply-add) x issues per cycle.
EE FPU: 1 lane. VU0/VU1 upper pipe: 4 lanes. The lower pipe is only for loads/stores here.
The clocks and the units' issue rules are taken from the manuals and measured before use.

Efficiency of the kernel, computed statically from `llc` output (no hardware needed):
useful multiply-adds / (cycles per element x peak per cycle). For the VU loops, use the cycle count
of the scheduled loop (the pipeliner's II remark). For the FPU, use the R5900 issue model's remark.
Hardware runs later confirm these numbers with the EE cycle counter.

## Steps

1. Build `wt/vu` and run lit (Part 1.1).
2. Fast-forward `r5900-feature`, add VU to the feature build, update CLAUDE.md (Part 1.2-1.3).
3. Write `vuk` and its checks.
4. Write `demo.c` and the three tests; run each with **ps2-pcsx2-test**.
5. Compute the static efficiency per unit; improve the VU codegen where it falls short (Part 1.4).
6. Later: hardware runs for the real numbers, one per "Is the PS2 ready?" confirmation.

## Open

- `vuk` confirmed by the user as the language (name and syntax).
- The VU helpers are written (`bench/vudemo/vu.h`).
- See "Still open" in the status section.
