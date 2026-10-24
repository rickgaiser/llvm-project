# VU entry functions: the contract between a frontend and the VU target

A frontend (clang's HLSL through `VUHLSLAdapter`, vuk with `--entry`, or a
SPIR-V translator) describes a micro program as one **entry function** and a
few **globals**. `VUEntryLowering` turns it into a micro program: it lays out VU
data memory, builds the loop over the items of a batch, and writes the layout
for the EE as a C header into section `.vu.header`
(`llvm-objcopy --dump-section .vu.header=<file>.h`). `VULowerMemory` then turns
the loads and stores into quadword accesses.

The pass runs at the start of the optimization pipeline (`opt`, clang) and
again in `llc` (it does nothing for an entry that is already lowered).

## The entry function

```llvm
define void @lvl2() #0 { ... }
attributes #0 = { "vu-entry"="vertex" }
```

* `"vu-entry"="vertex"`: a VU1 vertex program. The code is for one vertex.
* `"vu-entry"="compute"`: a VU0 or VU1 program over items. The code is for one
  item if it calls `llvm.vu.item.index`; otherwise it runs once per batch and
  loops over the items itself (`llvm.vu.item.count`).
* Calls are inlined. The function takes no arguments and returns `void`.

Other attributes:

| Attribute | Meaning | Default |
|---|---|---|
| `"target-cpu"` | `vu0` or `vu1` (vertex programs: `vu1`) | `vu1` |
| `"vu-buffer-qw"` | size of each of the two VU1 buffers of a vertex program | (1024 − spill) / 2 |
| `"vu-spill-qw"` | quadwords at the top of data memory for spilled registers | 0 |
| `"vu-header-prefix"` | prefix of the header's macro names | `VU_` |

## Pointers and memory

Pointers in every address space are 16-bit byte addresses of VU data memory.
Loads and stores must be by quadwords: every variable part of an address is a
multiple of 16 bytes, so `getelementptr <4 x float>, ptr %p, i16 %i` is fine
and an array of `float` indexed at run time is not. Within a quadword, a
constant offset selects the fields: `float`, `<2 x float>`, `<3 x float>`,
`i32` and vectors of them, and `i16` (the low half of a field, ILW/ISW).

## Globals, by address space

| AS | Kind | Metadata | Becomes |
|---|---|---|---|
| 1 | uniform | `!vu.uniform !{i32 <byte offset>}` | a fixed place in the uniform block; read only |
| 2 | vertex input | `!vu.input !{i32 <order>, !"<format>"}` | quadword `order` of the vertex's inputs |
| 3 | vertex output | `!vu.output !{i32 <order>, !"<GIF register>"}` | quadword `order` of the vertex's GIF registers |
| 4 | items | `[0 x T]`, `sizeof(T)` a multiple of 16 | the items of a compute batch |
| 5 | private | `!vu.carry !{}` keeps the value from item to item | a register (an alloca, promoted) |

A private global without `!vu.carry` starts each item with its initializer.

Formats (UNPACK, `vn_vl`; a `u` suffix zero-extends): `s32 s16 s8 v2_32 v2_16
v2_8 v3_32 v3_16 v3_8 v4_32 v4_16 v4_8 v4_5`. The program reads one quadword
per input: UNPACK has already turned the format into four 32-bit fields
(floats for `_32`, integers otherwise).

GIF registers (PACKED): `rgbaq st uv xyzf2 xyz2 fog xyzf3 xyz3`.

## Intrinsics

| Intrinsic | Value |
|---|---|
| `i16 @llvm.vu.item.index()` | the index of the item in the batch |
| `i16 @llvm.vu.item.count()` | the number of items in the batch |

## Layouts

**Vertex program** (VU1, double buffered: VIF1 BASE 0, OFFSET `BUF_QW`), at
XTOP:

```
0                GIF tag (from the EE; NLOOP = the vertex count)
1 ...            uniforms (UNI_QW quadwords)
IN_AT ...        inputs, ITEM_QW quadwords per vertex
OUT_AT           GIF tag (copied by the program)
OUT_AT + 1 ...   outputs, OUT_QW quadwords per vertex
```

The program ends with XGKICK of the outputs. `MAX_N` vertices fit a buffer;
batches start at multiples of `ALIGN` vertices, because UNPACK ends on a
quadword of every input stream.

**Compute program**, from quadword 0:

```
0          the item count (field x)
1 ...      uniforms
SIZE ...   the items, ITEM_QW quadwords each (sent and read back in place)
```

## The header

The macros (prefix `VU_` and the entry's name in upper case, `VU_LVL2_...`)
are the ones maxsdk's `MX_VU_SHADER` and `MX_VU_KERNEL` read: for a vertex
program `ENTRY BUF_QW UNI_QW ITEM_QW OUT_QW MAX_N ALIGN IN_AT OUT_AT NREG REGS
NIN IN_<input> INPUTS`, for a compute program `N <items> SIZE ENTRY ITEM_QW
FORMAT DBUF`, and for both the quadword of each uniform (`<uniform>`, with its
field in a comment if it does not start a quadword) and `SPILL_QW`.

## Spills

Spilled registers live in the top `vu-spill-qw` quadwords of data memory
(1023 down on VU1, 255 down on VU0). With none reserved, a program that needs
spills is an error that says how many it needs.

## HLSL (VUHLSLAdapter)

clang compiles HLSL for SPIR-V, whose output already has this shape; the
adapter (also at the start of the `opt` pipeline, and in `llc`) makes it a VU
module:

```
clang -cc1 -triple spirv-unknown-vulkan1.3-library -x hlsl -emit-llvm -O2 -finclude-default-header x.hlsl
opt -mtriple=vu -O2 -vu-cpu=vu0|vu1 [-vu-buffer-qw=N] [-vu-spill-qw=N] [-vu-header-prefix=P]
llc -mtriple=vu -mcpu=vu0|vu1 -O3 -filetype=obj
```

| clang's SPIR-V output | VU entry function |
|---|---|
| `"hlsl.shader"="vertex"` / `"compute"` | `"vu-entry"` |
| cbuffer members (address space 12, `!hlsl.cbs`, the layout struct) | uniforms at the layout's offsets, the cbuffers one after the other, each from a quadword |
| inputs (7), `Location` | inputs; the format from a semantic suffix (`COLOR_V4_8U`) or the type |
| `SV_VertexID` (built-in 42) | `llvm.vu.item.index` |
| outputs (8) `GIF_<register>[_<suffix>]` | outputs |
| `SV_Position` | an error: a vertex program writes GIF registers |
| the structured buffer (`resource.getpointer`, `getdimensions.x`) | the items; `llvm.vu.item.count` |
| `SV_DispatchThreadID` (`spv.thread.id` 0) | `llvm.vu.item.index` |
| a static (10) that is written | a private global with `!vu.carry` |
| `llvm.spv.*` math, `llvm.matrix.multiply` | generic IR |
| convergence tokens | removed |

After the entry lowering, `VUNarrowInt` does scalar integer operations wider
than 16 bits in 16 bits (the VI registers), and `VULowerMemory` makes the
loads and stores quadword accesses.

