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
| `"vu-buffer-qw"` | size of each of the two VU1 buffers of a vertex program | what the contexts and spills leave, / 2 |
| `"vu-contexts"` | contexts of a vertex program: 1 or 2 | 2 |
| `"vu-spill-qw"` | quadwords at the top of data memory for spilled registers | 0 |
| `"vu-header-prefix"` | prefix of the header's macro names | `VU_` |
| `"vu-indexed"` | an indexed vertex program of this many vertices per batch | 0 (not indexed) |

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
| 1 | uniform | `!vu.uniform !{i32 <byte offset>}` | a fixed place in the uniforms (of the context, for a vertex program); read only |
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

**Contexts** (vertex programs): what a draw sets once. `NCTX` of them, `CTX_QW`
quadwords each, from `CTX_AT`:

```
0                GIF tag (PRIM, FLG, NREG, REGS, EOP; NLOOP 0)
1 ...            uniforms (UNI_QW quadwords)
```

The EE writes a context once per draw, and the counts of each batch name it.
With two (the default) the EE uses them by turns, one per draw that has
batches: VIF1 gets to the context of draw d + 2 after the MSCALs of draw
d + 1, which wait until the batches of draw d have ended, so a context is not
written while a batch reads it. With one, the EE puts a FLUSHE in front of
it: the batches of the draw before end first.

**Vertex program** (VU1, double buffered: VIF1 BASE 0, OFFSET `BUF_QW`), at
XTOP:

```
0                counts (from the EE): x the vertex count, y the context
IN_AT ...        inputs, ITEM_QW quadwords per vertex
OUT_AT           GIF tag (the context's, with NLOOP = the vertex count)
OUT_AT + 1 ...   outputs, OUT_QW quadwords per vertex
```

The program ends with XGKICK of the outputs. `MAX_N` vertices fit a buffer;
batches start at multiples of `ALIGN` vertices, because UNPACK ends on a
quadword of every input stream.

**Indexed vertex program** (`"vu-indexed"="N"`): the code runs once for each
of the N vertices of a batch, in the order of the inputs, and writes the
outputs into a table; a gather then copies the outputs of each index into the
GIF packet, so a vertex that strips or triangles share is transformed once.
Each of the two buffers (VIF1 BASE 0, OFFSET `BUF_QW`, which the layout sets)
has, at XTOP:

```
0                counts (from the EE): x vertices, y the context, z index
                 quadwords, w indices (the packet's NLOOP)
IN_AT ...        inputs, ITEM_QW quadwords per vertex
IDX_AT ...       indices: UNPACK V4-8U, four to a quadword (sent as whole
                 quadwords of 16 bytes); each is the vertex times OUT_QW
```

and above both buffers, shared by the batches:

```
OUT_AT           GIF tag (copied by the program)
OUT_AT + 1 ...   outputs, OUT_QW quadwords per index (MAX_I of them)
EMPTY_AT         an empty GIF tag (NLOOP 0, EOP), written by the program
CTX_AT ...       the contexts
TAB_AT ...       the table: OUT_QW quadwords per vertex
                 (then the spills)
```

After the vertex loop the program sends the empty tag: XGKICK waits until
the GIF has read the packet of the batch before, so the gather can write the
one packet. The GIF reads a packet while the next batch's vertex loop runs.
The table is one for
both buffers: a batch writes and reads it before its XGKICK. Indices are
bytes: (N − 1) × OUT_QW ≤ 255. The index list may end with up to 3 more
indices than NLOOP (their outputs land after the packet), and strips restart
by repeating vertices (degenerate triangles); any primitive works, the gather
does not look at it. `SV_VertexID` is the vertex's number in the batch. A
private global with `!vu.carry` is an error: the vertices are not drawn in
the order they are transformed; so is `"vu-buffer-qw"`.

**Compute program**, from quadword 0:

```
0          the item count (field x)
1 ...      uniforms
SIZE ...   the items, ITEM_QW quadwords each (sent and read back in place)
```

## The header

The macros (prefix `VU_` and the entry's name in upper case, `VU_LVL2_...`)
are the ones maxsdk's `MX_VU_SHADER` and `MX_VU_KERNEL` read: for a vertex
program `ENTRY BUF_QW UNI_QW CTX_AT CTX_QW NCTX ITEM_QW OUT_QW MAX_N ALIGN
COUNTS IN_AT OUT_AT NREG REGS NIN IN_<input> INPUTS`, for an indexed one also
`INDEXED MAX_I EMPTY_AT TAB_AT IDX_AT` (OUT_AT is then an address of data
memory, not of the buffer), for a compute program `N <items> SIZE ENTRY
ITEM_QW FORMAT DBUF`, and for both the quadword of each uniform (`<uniform>`:
from 0 in the uniforms of a vertex program, in data memory for a compute
program; with its field in a comment if it does not start a quadword) and `SPILL_QW`.

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
opt -mtriple=vu -O2 -vu-cpu=vu0|vu1 [-vu-buffer-qw=N] [-vu-spill-qw=N] [-vu-header-prefix=P] [-vu-indexed=N] [-vu-contexts=1|2]
llc -mtriple=vu -mcpu=vu0|vu1 -O3 -filetype=obj
```

| clang's SPIR-V output | VU entry function |
|---|---|
| `"hlsl.shader"="vertex"` / `"compute"` | `"vu-entry"`; with `-vu-indexed=N`, `"vu-indexed"` on a vertex program |
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

