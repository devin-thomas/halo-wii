# Authored triangle surface word API

Clean source `e29f8ffdbbbf6cdc073013f56e27dc5b0ee324ef` passes the host authored diagnostic:
80 material cases / 50,484 checks and the retained address baseline 1,916 / 8,437,
with zero failures or aborts. Host build `15c3a1cdac2b73ae` executes; PPC build
`5aa6d83a4b07e6f1` compiles only. Both compile eight strict C11 units and finish
ten commands with zero exits. [Numeric evidence](2026-10-07-surface-api.json)
retains hash-only build, artifact, source and review identities.

The [getter](../../../tools/wii/cache_material_probe.c) resolves the copied
arena/BSP parent before reading child state or workspace. Each whole-root ordinal
returns three unsigned 16-bit words from a bounded six-byte LE source record.
Invalid ordinals, stale views and overlapping outputs fail before output changes.
The validated signed count retains its 131,072-record source maximum. Empty counts
ignore zero/NONE addresses while inspection serialization preserves source words.
Truthful disjoint buffers, immutable published inputs and one caller remain
[API preconditions](../../../tools/wii/cache_material_probe.h).

The [authored fixture](../../../tools/wii/cache_material_fixture.c) covers two
placements and source offsets zero through seven, unsigned extremes through 65,535,
full six-byte versus one-byte-short end spans, zero-count pointers, alias rejection,
rebind/unload and stale parents before overwritten child storage. It also exercises
the maximum 786,432-byte surface span and existing material/count limits. Eighteen
independent manual source assessments report no actionable issue; these assessments
are separate from fixture execution. The prior dirty preflight builds are retained
with their actual source commit and dirty status, not relabeled clean builds.

The root projection grows from 24 to 36 bytes. Two publication halves add 24 bytes
of workspace payload, changing the selected metadata request from 8,832 to 8,856
bytes. Inspection serialization still concatenates the opaque 648-byte root,
whole lightmap table and material records. It preserves the root surface
count/address/definition words without adding triangle payload. The historical
[owned material stream](2026-10-07-material-stream.md) result at source `b6f5899`
uses 9,416 selected bytes; this gate does not requalify those owned bytes.

## Numeric capacity model

Eight authored host planner invocations compare old/new layouts at zero and
4,096-byte placements. PPC structure widths come from a strict compiled object;
PPC requests execute as numeric models on truthful aligned host storage. Corrected
independent validation passes 182 checks. All twelve 64-byte-aligned slots retain
22 MiB tags, 20 MiB state, 4 MiB sound, 65,535 index records of 32 bytes, two 64 KiB
IO buffers, conservative widget workspace/serialization, separate BSP/material
controls and the provisional 2 MiB reserve. State/sound are placeholders.

| Layout model | First-placement charge | Alternate charge | Minimum available after reserve |
| --- | ---: | ---: | ---: |
| PPC numeric | 52,582,160 | 52,586,256 | 1,781,968 |
| Host native | 52,582,808 | 52,586,904 | 1,781,320 |

The original span is 54,368,224 bytes. Workspace growth adds 24 payload bytes and
40 alignment bytes, increasing charged use by 64 bytes. The alternate placement
charges its 4,096-byte prefix against the original span. PPC minimum available
space decreases from 1,782,032 to 1,781,968 bytes. This is an authored planner and
ABI model, not a repeated target heap/arena, peak or fragmentation measurement.

The first ignored planner harness omitted host GCC's bin directory from PATH and
failed setup. The second executed planner correctly used host BSP-control sizeof
200, while its reviewer expected 184 and reported one assertion failure. Both
records remain preserved; the final 182-check correction changes the validation
oracle without recompiling or reexecuting the planner. Neither is an API defect.

No new owned input, extraction, Dolphin launch, SD/settings change or target run
occurs. Exposed index words do not establish vertex indexing origin, per-material
index bounds or global ordering. Native vertices, triangle topology, float
arithmetic, shader bodies, GX/rendering, engine publication, gameplay and physical
Wii acceptance remain unqualified. Prior packet/ABI/shutdown failures remain
preserved. HWI-005/006 stay BLOCKED and HWI-007/008 stay TODO.
