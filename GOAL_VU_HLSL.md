# Goal: HLSL for the VUs (shaders and compute)

## Status (2026-10-24): P0-P4 done and checked on the PS2 (P5 optional, not planned)

| Phase | State |
|---|---|
| P0 VU target gaps | done: float compare/select as masks (`select.ll`), plain loads/stores (`VULowerMemory`, `memory-plain.ll`), spills to the top of data memory (`spill.ll`), floor/ceil/trunc/round/sin/cos/tan/exp/log/pow/atan/atan2/asin/acos from FMAC operations (`math.ll`; the EFU is not used: VU1 only, partial ranges, not pipelined); the call-in-loop hang was already fixed (`errors.ll`) |
| P1 IR contract + entry lowering | done: `VUEntryABI.md`, `VUEntryLowering` (opt pipeline start and llc), header in section `.vu.header`; vuk `--entry` gave the same code and headers for all 15 programs |
| P2 HLSL for VU | done, **changed** (see below): `VUHLSLAdapter`, `VUNarrowInt`, `VUSinkFields`, a VU TTI; lit `hlsl-adapter-*.ll`, `narrow-int.ll`, `entry-*.ll`, `program-size.ll` |
| P3 port every maxsdk `.vuk` | done: all 14 ported (`maxsdk/demos/*.hlsl`, `tests/ps2/floor.hlsl`, `nest.hlsl`); all 15 maxsdk PS2 tests pass on PCSX2, incl. the new `tests/ps2/shader2.c` (tf0-2, water, tree, dustdraw against the EE). Same screenshots: levels is identical to vuk's; procgen is identical to vuk's in both views when built with `-DPROCGEN_SAME`. The normal procgen build is not pixel-identical and cannot be for any faster code: its view distance, chunks and dust follow the frame times (vuk's own fbm, rebuilt faster with the same results, changes "looks" in 1395 pixels; see below). The user checked the results and accepted them (2026-10-24). Speed: see the table; every program is at or below vuk |
| P4 retire vuk, docs | done: maxsdk builds no `.vuk` (`VU_` names, `MX_VU_SHADER`/`MX_VU_KERNEL`), `maxsdk/docs/hlsl.md`, vuk kept as a test producer (`tools/vuk/vuk.md`) |
| P5 (optional) GLSL through SPIR-V | not planned |

Static cycles per item of the item loop (the MachinePipeliner's kernel, divided by the
iterations it does per pass), from `llc -debug-only=vu-loop-scheduler`, with vuk's
`--entry` IR compiled by the same llc:

| Program | HLSL | vuk | | Program | HLSL | vuk |
|---|---|---|---|---|---|---|
| lvl0 | 7.0 | 7.0 | | water | 103 | 168 |
| lvl1 | 17 | 17 | | tree | 31 | 52 |
| lvl2 | 20 | 20 | | dustdraw | 46 | 64 |
| lvl3 | 44 | 48 | | dust (VU0) | 45 | 46 |
| tf0 | 10.2 | 10.2 | | fbm (VU0) | 108 | 128 |
| tf1 | 21 | 21 | | floor (VU0) | 21 | 24 |
| tf2 | 25 | 25 | | nest (VU0) | 3.0 | 4 |

(lvl0, tf0 and nest run their last few items in the original loop after the kernel:
26, 35 and 19 cycles per item.) No kernel has a WAITQ word (see VUHazards below), which
these numbers would not show. Word counts were misleading: a loop that VULoopScheduler
rebuilt has its stalls as nops, one it left alone stalls without them.

What closed the gaps (LLVM, feature tree):
- MachinePipeliner: with the register pressure limit on, shortening the values of an
  iterative modulo schedule lowers the registers over the limit first (it used to only
  count kernel words, so every II was rejected for pressure: fbm 196 → 108 cycles, water,
  dust, dustdraw).
- VUPackConstants: an undefined vector base gets its IMPLICIT_DEF right before each use.
- VULoopScheduler: each length gets at most half of the steps left (lvl3, tf0).
- ACC sums: a last term that is a field of a register is added by a broadcast ADD instead
  of a splat and MADDw (tf1, water, nest; the fog `p.w * f.x + f.y` no longer takes ACC).
- ACC sums that may be reassociated start with the terms of values from outside the block
  (uniforms, constants), so a value computed in the loop is one MADD from the result:
  `amb + lcol * d` is MULAw, MADDx (not MULAx, MADDw: one FMAC latency longer). Terms
  computed in the block keep their order (moving them only holds ACC longer: fbm 108 → 117).
  `mul()` sums the columns with a constant factor first: `m3 + m0 * x + m1 * y + m2 * z`
  for w = 1, without reassociation (lvl0 7.2 → 7.0, lvl3 47 → 44, tf0, water, dustdraw).
- VUHazards: a loop that only waits for Q because of a DIV before it (the prologue of a
  pipelined loop) waits once on the way in, not in every iteration (lvl2: 21 → 20 words).
- Nondeterminism fixed: the VU DAG mutation kept the first of several artificial edges
  from one instruction (pointer order); the iterative scheduler's single-register overlap
  check and ModuloScheduleExpanderMVE (upstream) iterated pointer-keyed maps.

Screenshots. procgen's pictures depend on the speed of the units: the view distance follows
the frame times, chunks appear when VU0 has made their heights, the dust moves by the
vblanks that passed, and the HUD shows busy times. vuk's own fbm, compiled by today's llc
(the same results, bit for bit, but faster), already changes the "looks" picture in 1395
pixels. `-DPROCGEN_SAME` builds procgen without that (fixed view distance, the heights of a
chunk made in the frame that starts them, one vblank of dust per frame, no busy line); with
it, the HLSL build gives the same two pictures as vuk's programs. Two findings on the way:
- The vuk reference objects were from before the padding fix below: five of them (dust,
  lvl0, lvl1, tf2, water) had an odd number of words and lost their last word, so the old
  vuk pictures were wrong (dust skipped its first batch after fbm). The reference is vuk's
  IR compiled by today's llc.
- HLSL lets the compiler reassociate. `VUHLSLAdapter` sums the columns of `mul()` in a
  fixed order (no reassoc on those fadds): a vertex transform gives the same bits as
  `m3 + m0 * x + m1 * y + m2 * z`, the order vuk's reassociable IR gets from today's llc
  (cost of a fixed order: water 87 -> 96, dustdraw 46 -> 47 before the ACC changes). The phase sums
  of `dust.hlsl` and `water.hlsl` keep their order with a scoped
  `#pragma clang fp reassociate(off)` (dust 45 unchanged, water 96 -> 104), as fbm's hash
  does for the whole kernel.
Checked bit for bit on PCSX2 against vuk's programs: every VU1 shader over all batch sizes
(GS-visible fields; tf2's ST.w, which the GS ignores, is 0 in HLSL), fbm for 1-6 octaves and
any batch size, dust for 7300 frames.

Found on the way: programs of an odd number of words lost their last word in VIF's MPG (the
delay slot of the end ran another program's word): llc pads a program to quadwords now.

**P2 changed:** clang's HLSL output for SPIR-V (`-triple spirv-unknown-vulkan1.3-<stage>`)
already has the shape of the contract (semantic globals, cbuffer layout, a small set of
`llvm.spv.*` intrinsics). An IR adapter pass in the VU target turns it into the contract,
instead of a VU branch in clang's HLSL code generation (about 60 DXIL/SPIR-V switch points in
code that changes fast upstream). The PS2 additions stay HLSL: formats and GIF registers as
semantics (`COLOR_V4_8U`, `GIF_XYZ2`), the unit and buffer size as compile options, and a
written `static` keeps its value from item to item (C semantics).

## Goal

Write VU programs in **HLSL**, compiled by clang. VU1 vertex shaders and VU0/VU1 compute kernels
both come out as fast as vuk does today.

1. A shader author can read and write them as HLSL. `cbuffer`, `float4x4`, `mul`, semantics,
   `[shader("vertex")]` and `[numthreads]` all work as in HLSL. Where the PS2 needs something HLSL
   lacks (UNPACK formats, GIF registers, fixed point, VU0/VU1), add it in HLSL style: `[[vu::...]]`
   attributes, `GIF_*` semantics and `vu::` intrinsics. Never add new statement syntax.
2. The batch loop, `xtop` double buffering, the GIF tag, XGKICK and the EE layout header are
   generated by LLVM, not by a frontend script. Any frontend (HLSL now, GLSL/SPIR-V later, vuk as a
   test) produces the same per-item IR.
3. Every vuk program in `maxsdk/demos` is rewritten in HLSL. Each one gives the same output and
   runs at the same speed or faster.
4. vuk is retired (or kept only as a test producer).

GLSL is not part of this goal. clang has no GLSL frontend; the route would be glslang → SPIR-V →
LLVM IR → the same contract (P5, only if there is a need).

## Rules

- These are the same rules as in GOAL_VU_DEMO.md and `../CLAUDE.md`. Work in the feature tree
  (`llvm-project/`, build `toolchain/build/llvm`). Commits only with the user's approval, no
  co-author line, `git clang-format`. Use `update_llc_test_checks.py` /
  `update_cc_test_checks.py`, not hand-written `CHECK-NOT`.
- **Fix LLVM, no workarounds.** If an HLSL program compiles badly, fix the compiler. Don't
  rewrite the shader around the problem.
- **PCSX2 only, no real PS2 while working on this goal.** Check correctness with the
  ps2-pcsx2-test skill (TGA screenshots for the demos). The PS2 runs come after all phases are
  done (see "After the goal"). PCSX2 timings are not performance data, so speed is judged by the
  static numbers from `llc` only.
- **Reference for speed:** the vuk version of each program, using its cycles per vertex or item
  from `llc` (static). An HLSL port is done when it is ≤ the vuk number and gives the same pixels
  or results on PCSX2.
- Use R5900 in code, tests and commits, not "EE", except for the EE as a processor next to the VUs.

## Key facts (checked in this tree)

| Fact | Where |
|---|---|
| clang has a full HLSL frontend: `SemaHLSL.cpp` (7.3k lines), `CGHLSLRuntime.cpp` (2.5k), headers `clang/lib/Headers/hlsl/` (vector/matrix types, intrinsics, resources) | clang |
| HLSL is accepted only for DXIL and SPIR-V. The triple OS must be `shadermodelX.Y` (or Vulkan for logical SPIR-V), and the environment is the shader stage | `CompilerInvocation.cpp:4678`, `Driver.cpp:3857` |
| clang lowers HLSL intrinsics to target intrinsics: `GENERATE_HLSL_INTRINSIC_FUNCTION(Frac, frac)` → `llvm.dx.frac` / `llvm.spv.frac`. A VU target needs a third postfix (`llvm.vu.*`) or generic IR (`fdiv`, `llvm.sqrt`, `llvm.floor`) | `CGHLSLRuntime.h:37-170` |
| `emitEntryFunction` wraps the user's entry in a stage entry. Inputs and outputs go through semantics (`emitUserSemanticStore`, `emitSystemSemanticStore`), split by `isDXIL()`/`isSPIRV()` | `CGHLSLRuntime.cpp:1441-1919` |
| The attributes this goal needs already have models: `HLSLParsedSemantic`, `HLSLNumThreads`, `HLSLShader`, `HLSLPackOffset`, `HLSLVkLocation`/`HLSLVkBinding` (`[[vk::...]]`, the model for `[[vu::...]]`) | `clang/include/clang/Basic/Attr.td` |
| cbuffer layout follows HLSL packing rules (`HLSLBufferLayoutBuilder.cpp`). A `float` array uses one 16-byte register per element. That matches the VU's quadword addressing, but not vuk's packed `f32[K]` tables | clang |
| VU target input: `define void @k()` without arguments. Memory goes through `llvm.vu.lq/sq/sq.mask/ilw/isw`, VU1 also `llvm.vu.xtop/xitop/xgkick`. FMAC/ACC chains, Q (`fdiv`, `sqrt`), modulo scheduling and dual-issue packing all work | `llvm/lib/Target/VU` |
| vuk (`maxsdk/tools/vuk/vuk.py`, 1005 lines) generates in Python what P1 moves into LLVM: the batch loop, `xtop`, the GIF tag at quadword 0 (NLOOP from the EE), uniforms from quadword 1, inputs at `IN_AT`, outputs at `OUT_AT`, alias scopes in/out, `state` phis, XGKICK, and the header (`ENTRY`, `BUF_QW`, `UNI_QW`, `ITEM_QW`, `OUT_QW`, `MAX_N`, `ALIGN`, `IN_AT`, `OUT_AT`, `REGS`, `INPUTS`) | `compile_shader`, `compile_kernel` |
| vuk programs to port: shaders `lvl0-3`, `tf0-2`, `water`, `tree`, `dustdraw`; kernels `fbm`, `dust` (VU0) | `maxsdk/demos/*.vuk` |

## The language: HLSL with a `vu` profile

### A vertex shader (lvl2.vuk in HLSL)

```hlsl
// Quality level 2: level 1 textured, perspective correct.
#include <vu/ps2.hlsli>              // GIF_* semantics, vu:: intrinsics, unpack formats

cbuffer Frame {                      // uniforms, per batch, from quadword 1
    float4x4 mvp;                    // column_major: one quadword per column
    float4   light;
    float4   lcol;
    float4   amb;
};

struct VSIn {
    float3                           pos : POSITION;   // V3-32 (the default for float3)
    [[vu::unpack(V4_8)]]   int4      nrm : NORMAL;
    [[vu::unpack(V4_8U)]]  uint4     col : COLOR;
    [[vu::unpack(V2_16)]]  int2      uv  : TEXCOORD0;  // 4.12 fixed point
};

struct VSOut {                       // one GIF register per field, in this order
    float4 st    : GIF_ST;
    int4   rgbaq : GIF_RGBAQ;
    int4   xyz2  : GIF_XYZ2;
};

[shader("vertex")]
VSOut lvl2(VSIn v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    float  q = rcp(p.w);
    float  d = max(dot(float3(v.nrm), light.xyz), 0);
    VSOut o;
    o.st    = float4(vu::itof12(v.uv) * q, q, 0);
    o.rgbaq = int4(min(float4(v.col) * (amb + lcol * d), 255));   // FTOI0
    o.xyz2  = vu::ftoi4(p * q);
    return o;
}
```

### A compute kernel (dust.vuk in HLSL)

```hlsl
#include <vu/ps2.hlsli>

cbuffer Params { float4 c; };        // dt * speed, time, swirl, sink * dt
RWStructuredBuffer<float4> x;        // the motes, in VU data memory

[shader("compute")]
[numthreads(1, 1, 1)]
[[vu::unit(vu0)]]
void dust(uint i : SV_DispatchThreadID) {
    float4 p = x[i];
    float3 s = sin(float3(p.z * 9.0 + c.y * 0.9 + p.w,
                          p.x * 7.0 - c.y * 0.6 + p.w * 1.3,
                          (p.x + p.z) * 5.0 + c.y * 1.1));
    float3 d = float3(s.x * c.z + 0.4, s.z * 0.3, s.y * c.z + 0.2) * c.x - float3(0, c.w, 0);
    x[i] = float4(p.xyz + d, p.w);
}
```

### PS2 additions (proposed; settle in P2)

| Need | HLSL-style form | vuk today |
|---|---|---|
| UNPACK format of an input | `[[vu::unpack(V4_8U)]]` on a field. Defaults: float4 → V4_32, float3 → V3_32, float2 → V2_32 | `in col: v4_8u` |
| GIF register outputs | semantics `GIF_ST`, `GIF_RGBAQ`, `GIF_XYZ2`, `GIF_XYZF2`, `GIF_UV`, `GIF_FOG`, ... The field order is the REGLIST order | `out rgbaq = ...` |
| Skip a vertex (ADC) | `bool skip : GIF_ADC` (sets bit 15 of XYZ[F]2's w), or written into w by hand as in `water.vuk` | by hand |
| Fixed point | `vu::itof4/12/15`, `vu::ftoi4/12/15`; `int4(f)`/`float4(i)` are FTOI0/ITOF0 | `itof12`, `ftoi4` |
| State carried from vertex to vertex (strips) | `[[vu::carry]] static float2 exs = 0;`: keeps its value from one vertex to the next, reset to the initializer at each batch | `state exs = 0;` |
| VU0 or VU1 | `[[vu::unit(vu0)]]`, or the triple (`vu0-…`/`vu1-…`) | `unit vu0;` |
| EFU (VU1) | standard `sin`, `cos`, `exp`, `atan`, `rsqrt`, `sqrt`. These select ESIN/EEXP/EATAN/ERSQRT on VU1 and a polynomial on VU0 | `sin` (polynomial) |
| Buffer size | `[[vu::buffer(496)]]` on the entry, or a driver flag | `-b` |

Standard HLSL that has no VU meaning (pixel/geometry stages, textures, samplers, waves, groupshared
across threads) is rejected with a diagnostic.

### Driver

Recommended: reuse clang's shader-model triple form, so that the existing HLSL driver and Sema
paths apply: `clang --target=vu1-sony-shadermodel6.0-vertex -x hlsl -E lvl2 lvl2.hlsl -c -o lvl2.o`
(or `clang-dxc -T vs_6_0 --target=vu1 …`). The object's `.vutext` and the layout go into maxsdk as
the `.vuk` outputs do today. Decide in P2 whether the triple OS stays `shadermodel` or becomes
`ps2`.

## Architecture

```
HLSL ──clang (CGHLSLRuntime, vu branch)──┐
vuk  ──vuk.py (per-item mode, for tests)─┼─> per-item IR + vu metadata ──VUEntryLowering──> today's VU IR ──llc──> .vutext + layout
GLSL ──glslang → SPIR-V → LLVM (P5)──────┘
```

**The per-item IR contract (P1).** A frontend emits:
- one function for one item (vertex or compute index), marked `"vu-entry"="vertex"|"compute"`;
- inputs, uniforms and outputs as pointer arguments in VU address spaces (uniform, input stream,
  output stream, buffer). Their formats (UNPACK, GIF register, ADC) go in metadata;
- carried state as a marked global;
- plain `load`/`store`, generic float IR and `llvm.vu.*` only where no generic form exists.

**VUEntryLowering** (new VU pass, early in `llc` or as an `opt` pass) builds what `compile_shader` /
`compile_kernel` build today: the data memory layout (GIF tag, uniforms, aligned inputs, outputs,
`MAX_N` for the buffer), the `xtop` loop with the alias scopes in/out, the phis of the state, the
GIF tag and XGKICK. It inlines everything into one `define void @k()`.

**The layout for the EE.** The pass records the layout in the object (a `.vu.layout` section or
absolute symbols `lvl2.MAX_N`, …). A small tool, or a clang flag (decide in P1), writes the C header
with the same `VUK_*`-style names that `<mx/frame.h>` uses (`mx_frame_shader_*`), so the maxsdk side
does not change.

## Phases

| Phase | Work | Done when |
|---|---|---|
| **P0 VU target gaps** | Fix the hang after the call diagnostic in a loop; float `SETCC`/`SELECT`/`BR_CC` (MAC/status flags, CLIP, MIN/MAX); loads at a computed index (`ftoi` → `mtir` → `ilw`/`lq`); EFU selection on VU1; plain `load`/`store` in VU address spaces | lit tests in `CodeGen/VU` for each; vuk programs unchanged in cycles |
| **P1 IR contract + VUEntryLowering** | Write the contract down (`llvm/docs` or `VU.md`). Implement the pass and the layout record + header writer. Add a per-item output mode to vuk and switch maxsdk to it | every `.vuk` through the new path gives the same `llc` output (or equal cycles) and the same header; `tests/ps2/shader.c` passes on PCSX2 |
| **P2 clang HLSL for VU** | `vu` TargetInfo (`vu0`, `vu1`; data layout; `float4` = 16 bytes); allow HLSL for arch `vu`; a `vu` branch in CGHLSLRuntime (entry, semantics → contract, cbuffer → uniform space, `RWStructuredBuffer` → buffer space); intrinsic postfix `vu` or generic IR; `vu/ps2.hlsli`; the `[[vu::...]]` attributes and `GIF_*` semantics; diagnostics for unsupported HLSL | `clang/test/CodeGenHLSL/vu/*` and `SemaHLSL/vu/*`; lvl2 and dust compile to the same cycles as vuk |
| **P3 port maxsdk** | Rewrite `lvl0-3`, `tf0-2`, `water`, `tree`, `dustdraw`, `fbm`, `dust` in HLSL; maxsdk build rules for `.hlsl` | each one ≤ vuk cycles (static); same screenshots on PCSX2 (procgen: under `-DPROCGEN_SAME`; the normal build follows the frame times, checked and accepted by the user, 2026-10-24); procgen runs on PCSX2 |
| **P4 retire vuk** | maxsdk docs (`docs/vu1.md`), a HLSL-for-VU language page; remove vuk from the build or keep it as a test producer | user confirms |
| P5 (optional) | GLSL: glslang → SPIR-V → `llvm-spirv` → the contract | only if asked |

Order: P0 and P1 come first, because they help vuk at once and fix the contract before any clang
work. P2 is the largest part.

### After the goal: real PS2 runs

Done 2026-10-24 (ps2-hw-test): all 15 maxsdk tests pass on the PS2, and levels and procgen
run. levels, cycles per vertex ("A small", 139136 vertices), HLSL against vuk's IR compiled
by the same llc, both measured on the PS2 in the same session:

| | lvl0 | lvl1 | lvl2 | lvl3 | lvl4 |
|---|---|---|---|---|---|
| HLSL | 9.68 | 18.60 | 21.96 | 46.75 | 12.23 |
| vuk | 9.68 | 18.59 | 22.06 | 50.64 | 12.23 |

lvl1 is the same code apart from registers; a second pair of runs gave 2588021 (HLSL) and
2588063 (vuk) cycles per frame, so the 0.01 is noise (about ±400 cycles per frame).

Against the criterion above: PLAN_PROCGEN.md's L1-L3 of its status table (17.67, 20.87,
40.61) are from before "Lit colours clamped to 255", which the same file records as
L1 18.62, L2 23.10, L3 53.71 (and fBm 446 → ~787 after the exact floor). Those are the vuk
numbers to meet, and the old vuk objects give them again on today's PS2 run (9.89, 18.62,
23.10, 53.71). HLSL: 9.68, 18.60, 21.96, 46.75, all lower; fBm in `tests/ps2/noise.c`: 664
VU0 cycles per sample at 6 octaves including DMA.

The first hardware runs found what the static per-item cycles did not show: HLSL's lvl1
had a one stage deeper pipeline (the ACC order of `amb + lcol * d`) and lvl2 a WAITQ word in
its kernel (the prologue's DIV). Both are fixed in LLVM (see "What closed the gaps").

procgen: "looks" 7200 frames in 7199 vsyncs, 0 missed; "max count" 21.95 M vertices/s
(≥ 20.26). Screenshots: levels on the PS2 differs from PCSX2 by at most 4 levels per
channel (GS rounding); procgen's pictures show the same scene, with the timing-dependent
differences described above.

## Open decisions (ask the user when reached)

1. Triple: `vu1-sony-shadermodel6.0-vertex` (reuses the HLSL driver) or a `ps2` OS.
2. Layout record: a section + tool, absolute symbols, or a clang `-fvu-header=` flag.
3. `float` arrays in a cbuffer: HLSL packing (one quadword each) or a `[[vu::packed]]` opt-in for
   vuk-style tables.
4. Matrix default: `column_major` (one quadword per column, as `mvp[0..3]` in vuk) is assumed.
5. Upstream: the VU target is experimental and out of tree for now. Keep the clang side separable
   (one TargetInfo, one CGHLSLRuntime branch) in case it is ever proposed.

## Risks

- **CGHLSLRuntime assumes DXIL or SPIR-V** in many places (resources, semantics, buffer layout
  types). A third target may touch many functions. Mitigation: map VU onto the SPIR-V-like path
  where possible, and keep the `vu` branch small.
- **Generic IR from clang may not hit the patterns vuk's IR hits** (e.g. `mul(float4x4, float4)` as
  dot products instead of an ACC chain, `min(..., 255)` with a NaN-correct form). Fix it in the VU
  target (combines), and check the cycles of every port against vuk.
- **HLSL `float` semantics** (IEEE, NaN) do not match the VU. Compile with fast-math flags by
  default for `vu` (no NaN/inf, as the hardware), but watch reassociation (see GOAL_VU_DEMO.md:
  reassociation into longer chains is an open compiler issue).
- **No spilling on the VU:** large HLSL shaders (many lights) fail. The pipeliner's register
  pressure margin helps in loops only.
