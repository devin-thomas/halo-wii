# Selected owned BSP residency diagnostic

Source `899f860cb0c8ee4d7605e1fa736f6ebd4d0885c8` passes bounded BSP
association, header, opaque root extent and lifetime checks on host and
emulated PowerPC. Both stock Dolphin launches complete guest result zero,
then fault naturally with host `0xc0000409`. Host shutdown remains blocked.
[Numeric evidence](2026-10-07-owned-bsp-residency.json) records source,
compiler, SDK, artifact and raw-record identities without asset bytes or
private paths. This extends the [widget graph](2026-10-07-owned-widget-graph.md)
and [streaming](2026-10-07-owned-cache-stream.md) diagnostics.

## Scope and address windows

The selected UI scenario reference identifies a 2,048-byte BSP at logical
map offset 2,048, encoded base `819a5800`, full datum `e54c03d6`. A bounded
inflate produces only that prefix: 65,536 compressed bytes read, 576 consumed,
2,048 produced, no EOF. The stored map's identity matches prior inspection;
this step does not revalidate the complete stream or canonical checksum.
Raw BSP CRC is `75fb1554`; SHA-256 is
`ba61e5950985de7351e4f7f0c073bd654084f2104902e1d284d99b3de8573bc8`.

The source declarations are [cache_files.c](../../../source/cache/cache_files.c),
[scenario_definitions.h](../../../source/scenario/scenario_definitions.h),
[tag_schema_scenario.c](../../../port/linux/game/tag_schema_scenario.c),
[cache_file_formats.c](../../../port/linux/game/cache_file_formats.c) and
[tag_validate.c](../../../port/linux/game/tag_validate.c). Scenario root size
is 1,456; its BSP block starts at 1,444. Reference size is 32; BSP header is
24; the root extent is 648. The runtime supports 16 BSP references; the
standalone format's 32-entry limit does not supersede that runtime limit.

Full unsigned datum/group identity and signed counts/file bounds are checked.
Logical file extent must fit the declared map; the 512-rounded reservation
must fit the unchanged 22 MiB tag slot above actual tag bytes. The two readable
windows are 1,642,244 actual tag bytes at `803a6000` and 2,048 actual BSP bytes
at `819a5800`. The intervening 21,424,380-byte gap and reservation tail are
inaccessible. Nonempty spans must fit one window; empty arrays do not follow
unused pointers. The header's vertex/index descriptor counts are both zero
in this actual input. Positive descriptor coverage comes from authored tests.
Descriptor stride is 12; Data addresses are bounded for one byte only, without
proof of the complete geometry payload length. The 648-byte root stays opaque.

The caller stages bytes before acquiring views. A native child control lives
in a charged parent-owned slot. Each view copies the persistent parent pin;
parent validity is checked before reading child control storage, then the
child epoch is checked. Failed binds preserve prior control/view/source.
Two child loads per parent cycle distinguish unload, child rebind, parent
release and parent rebind. This requires truthful disjoint control/output
storage, an immutable pinned source and one caller. Engine root publication,
unload/switch behavior and nested geometry conversion are not executed.

Serialization copies the whole declared BSP, preserving unknown bytes and
explicitly rewriting known LE header/descriptor words. Encoded addresses stay
unchanged; the result is inspection output, not a relocated cache or save.
The target computes CRC; the matching SHA-256 is an independent expected hash.

## Verification and charged memory

Clean host/PPC builds compile 12 strict C11 units with O2, Wall, Wextra,
Werror and stack-usage output. All 14 commands per build exit zero with no
compile diagnostics. Host GCC 16.2.0 build is `275a503eae398546`; PPC GCC
16.1.0 build is `83ef1a6ffb134078`. Generated headers contain numeric goldens
and SD paths, without embedded asset arrays. The asset-free fixture and
36-test Python inspector suite are documented in [the README](../../../tools/wii/README.md).

Both executions pass BSP 75 cases / 5,432 checks, widget 189 / 5,521,
address 1,916 / 8,437 and arena 1,407 / 2,981 with zero failures or aborts.
Arena counts depend on alignment. Host tmpfile IO passes 60 / 958; that
fixture explicitly does not execute on PPC. Independent authored checking
adds 16,095 checks, including an overwritten released child control with
parent-first rejection. Tests cover signed/overflow/window/overlap, datum/
group association, descriptor bounds, failed atomic publication and lifetime.

Actual host/PPC each finish 16 parent cycles at two placements, 32 child loads,
16 child-release and 16 child-rebind STATE rejections, 16 parent-release and
15 parent-rebind STATE rejections. Owner generation ends at 32. All 983 tags
stream through 26 chunks / 27 fread calls; BSP through one chunk / two calls.
The earlier widget still serializes 2,160 bytes with CRC `1f36702b`.

| Reservation/measurement | Host | PPC |
|---|---:|---:|
| Widget workspace | 2,048 | 1,456 |
| BSP stage / serialized slots | 2,048 / 2,048 | 2,048 / 2,048 |
| Native BSP control / view | 200 / 40 | 152 / 40 |
| Data end | 50,471,240 | 50,470,616 |
| Required including provisional 2 MiB reserve | 52,568,392 | 52,567,768 |
| Maximum charge including alternate 4 KiB placement | 52,572,488 | 52,571,864 |
| Remaining in measured 54,368,224-byte MEM2 span | 1,795,736 | 1,796,360 |

The resident BSP already occupies the existing tag slot. Full reservations
remain tags 22 MiB, state 20 MiB, sound 4 MiB, 65,535 index records of 32 bytes,
two 64 KiB IO buffers and a provisional 2 MiB reserve. State/sound are edge
canary placeholders, not initialized engine pools. Textures, actual pool
metadata, geometry conversion and full stack/system peak are excluded.
Three controlled intervals totaling 54,237,152 bytes (largest 18,122,720)
reject the contiguous PPC request with guards intact. This measures owned
interval constraints, not system allocator fragmentation.

## Retained failures and remaining gates

The initial host preflight fails compilation because generated `CACHE_BSP_DATUM`
collides with the error enum. Renaming generated macros to `CACHE_BSP_GOLDEN_*`
fixes it. The failed argv, generated header, diagnostics and prior objects
remain private. Historical failed main/runner full sources were not snapshotted;
the coordinator's independent two-include replay reproduces the collision,
without claiming an exact historical main rebuild. Separately, source review
finds omitted tag-header descriptor overlap claims; the fix adds 14 negative
cases. That review gap is distinct from the executed compile failure.

Two stock Dolphin 2609/D3D launches use the same immutable DOL SHA-256
`83218d58a17033dacf6dfc5491157d9f47eb362100ee834727a823bf848aebf5`.
Both complete identical BEGIN/END guest reports with result zero, then fault
naturally with `0xc0000409`: root 12.476 seconds, independent reproduction
11.3900892 seconds, no timeout or kill. LF guest SHA-256 is
`2cc8f14d275ba5fdd36785f5e76bb5a010a0ee9006593302241b37cd965f227d`.
This records a guest pass and failed host exit; earlier clean diagnostic
exits and earlier faults remain separate. No settings matrix was attempted.

The root run preserves all 16 prior SD files and both private inputs;
only `cache-bsp.log` is new. Settings remain unchanged. MEM2 bounds
`90002000..933db7e0` restore exactly; MEM1 after SD setup is
`8016d000..81800000` (23,670,784 bytes). Heap in-use stays 783,272 bytes,
but arena grows 813,408 -> 850,272 and free 30,136 -> 67,000. This does not
prove full heap-state restoration or peak memory. Read-only stat guards and
before/after equality do not constitute locks or exclude transient writers.

Independent retained audit passes 384 consistency checks with zero findings:
frozen build inputs, current SDK/compiler/artifacts, ELF/DOL payload, numeric
goldens, budgets and fresh read-only FAT agree. The current checkout contains
the expected publication drafts; immutable build inputs match the source
commit. The FAT log contains two identical retained guest segments. This audit
adds no runtime launch or SD write and retains both host qualification failures.

HWI-005/006 remain BLOCKED and HWI-007/008 remain TODO. Engine memory strategy,
nested BSP geometry, rendering, gameplay and physical Wii remain open. Next
preparation should inspect a bounded campaign BSP with nonempty descriptors
and root block counts before choosing conversion workspace or native schemas.
Host shutdown diagnosis remains a separate matching-symbol callback-owner gate.
