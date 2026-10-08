# Representative owned UI widget graph

Source `0207d74e8408d85b848d215140ac5f76b6266c56` passes a bounded partial
typed graph on host and emulated PowerPC. This extends the
[owner/streaming diagnostic](2026-10-07-owned-cache-stream.md); it does not
qualify the engine memory strategy, full UI conversion, event execution or gameplay.
[Numeric evidence](2026-10-07-owned-widget-graph.json) retains source, compiler,
SDK, artifact and raw-record identities without asset bytes or private paths.

## Scope and schema

Independent numeric inspection checks 485 `DeLa` root extents and their five
signed block counts/nonempty spans (3,803 checks, zero failures). Full traversal
is limited to root ordinals 407 -> 408: two 1,004-byte roots, one 72-byte event
and one 80-byte child, totaling 2,160 serialized bytes. The selected graph has
15 references: 12 NONE and three nonnull. Other resource bodies are not loaded.
The 485-root extent scan does not prove all 485 graphs.

The source declarations are [ui_widget.c](../../../source/interface/ui_widget.c),
[tag_schema_effects.c](../../../port/linux/game/tag_schema_effects.c) and
[tag_validate.c](../../../port/linux/game/tag_validate.c). Block offsets are
72/84/96/724/992; strides 36/72/34/80/80; validator maxima 64/32/32/32/32.
Observed maxima across the roots are 2/13/1/1/9. These are source/tool limits,
not an independently qualified deployed Xbox hard-cap policy. Two native nodes
are the selected workload budget; original tag/state/sound/index capacities remain.

Named numeric fields use explicit LE reads/writes. Full unsigned datums and
expected groups are checked. NONE references retain group/name words without
following meaningless name pointers; zero nonnull name lengths are accepted
as in the source validator. Known strings receive bounded NUL checks. Float
values stay as uint32 bits. Unidentified bytes remain opaque and unchanged.
DFS visits children first and the extended-description edge only for column
list type 3. Cycles reject; DAG reuse and a 32-edge/33-node path are supported.

The native projection has separate published/scratch workspace halves; failed
decode leaves the published graph unchanged. Owner epoch checks precede any
stale workspace access. This requires an immutable, pinned, single-caller
source and disjoint control/workspace/output storage. Serialization copies
visited opaque spans and explicitly rewrites known numeric fields. It keeps
original encoded addresses, so the result is inspection concatenation rather
than a relocated standalone cache or durable save.

## Verification and measured budget

Clean host/PPC builds compile ten units with C11, O2, Wall, Wextra, Werror and
stack-usage output: host GCC 16.2.0 build `a0c0ba95bcd3014a`; PPC GCC 16.1.0
build `33cf2463c49d6b96`. All twelve commands per build exit zero without
compile diagnostics. Compiler overrides are cleared and SDK identity retained.
Generated headers contain numeric goldens and an SD input path, without raw
asset arrays. Asset-free `--widget-fixture` and the 30-test Python inspector
suite are documented in [the tool README](../../../tools/wii/README.md).

Both executions pass 189 widget cases / 5,521 checks, address 1,916 / 8,437 and
arena 1,407 / 2,981 with zero failures or aborts. Arena counts can depend on
actual alignment. Host tmpfile IO passes 60 / 958; that fixture is explicitly
not executed on PPC. Authored widget tests cover all five block maxima,
negative offsets/bounds, nonzero float bits (negative zero, subnormal, NaN
payload and infinity), DAG/cycle/depth, malformed atomic publication, source
immutability, alias/capacity/guards and repeated lifetimes. Owned color and
child-offset values are zero; synthetic nonzero coverage is separate.

Actual host/PPC each complete 16 cycles at two placements. All 983 input tags
stream through 26 synchronous 64 KiB chunks / 27 fread calls. The graph always
has two nodes/two elements and reserializes 2,160 bytes with CRC `1f36702b`.
Independent expected serialization SHA-256 is
`d65eaa9ba2590f98848781b80555fab182b9f5febc5add567a458f7bc7d466c5`;
target runtime computes CRC, not SHA-256.
Deliberate short/trailing/CRC errors precede graph acquisition. Stale graphs
reject with STATE after 15 rebinds and 16 releases; owner generation ends at 32.
Raw CRC proves input identity, not canonical Xbox checksum correctness.

| Reservation/measurement | Host | PPC |
|---|---:|---:|
| Native projection bytes | 512 | 364 |
| Graph workspace bytes | 2,048 | 1,456 |
| Serialized slot bytes | 2,160 | 2,160 |
| Data end bytes | 50,466,928 | 50,466,352 |
| Required incl. provisional 2 MiB reserve | 52,564,080 | 52,563,504 |
| Max charge incl. alternate 4 KiB placement | 52,568,176 | 52,567,600 |
| Remaining in measured 54,368,224-byte MEM2 span | 1,800,048 | 1,800,624 |

Full reservations stay tags 22 MiB, state 20 MiB, sound 4 MiB, 65,535 index
records of 32 bytes and two 64 KiB IO buffers. State/sound are edge-canary
placeholders, not initialized engine pools. Projection/serialization slots
are separately charged. Textures, engine pool metadata, full UI/BSP/resource
conversion, library/call-chain stack and system peak remain excluded.
Three authored intervals totaling 54,237,152 bytes (largest 18,122,720) reject
the 52,563,504-byte PPC contiguous request with guards intact; this measures
owned interval constraints, not system allocator fragmentation.

## Scoped runtime and remaining gates

One stock Dolphin 2609/D3D run with the existing single-core profile exits
naturally with host status zero after 9.691 seconds, no timeout, and guest
result zero. DOL SHA-256 is
`8253e007999fd7ad4a24406ca560c7882d1eb7f9c2dd0f51331bceb3a98c388b`;
LF guest SHA-256 is
`be180a3714b1e4a3919f762c6b5768ec07a5ed6ad0ce0b6263d285bfef7f2989`.
All 14 prior SD files, the 1,642,244-byte private input and profile settings
remain unchanged; only `cache-widget.log` is new. MEM2 bounds
`90002000..933db7e0` restore exactly. MEM1 after SD setup is
`80160000..81800000` (23,724,032 bytes). Heap in-use remains 783,272 bytes,
but arena grows 785,376 -> 822,240 and free 2,104 -> 38,968. This is not
full heap-state/peak restoration or general shutdown recovery.

Independent retained audit passes 291 checks with zero findings: clean source
and build identities, artifacts, ELF/DOL layout, numeric header, representative
raw spans, budgets/lifetimes, fresh read-only FAT input/prior files, guest and
settings agree. It adds no target run or SD write. Before/after equality and
the inspector's read-only stat guard do not exclude transient writers or
constitute a filesystem lock. Existing shutdown faults and seven host/nine
PPC native ABI gaps remain. HWI-005/006 stay BLOCKED; HWI-007/008 stay TODO.
Next preparation addresses actual BSP encoded regions and residency/lifetime;
full conversion, rendering, event execution, physical Wii and gameplay are open.
