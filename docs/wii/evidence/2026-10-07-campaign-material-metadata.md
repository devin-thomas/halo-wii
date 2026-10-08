# Retained campaign material metadata

Read-only inspection at source `c2c871c6393d03f8b0117fcf5f6acfb1def6b86c`
extends the [campaign BSP sample](2026-10-07-campaign-bsp-inspection.md) through
two lightmaps and all 34 materials. [Numeric evidence](2026-10-07-campaign-material-metadata.json)
records measured metadata and a memory proposal without asset bytes or paths.
No additional extraction, target run, SD write or native decoder is performed.

## Measured fields and relationships

The lightmaps contain 20 and 14 materials; both bitmap indexes are -1.
All materials have compressed environment type 1 and compressed lightmap
type 3. Environment counts range from 4 to 5,863, totaling 14,788; all
lightmap vertex counts are zero. Compressed tag-data sizes total 473,216 bytes;
uncompressed sizes are zero. Material surface ranges are ordered/disjoint
and total 8,084, equal to the selected root's surface count. Surface triangle
indices and vertex contents are not inspected.

All 68 hardware-format words identify records in their correct 34-entry,
12-byte header tables with record alignment. Main vertices use the vertex
table; lightmap vertices use the index table. Descriptor Data at +4 supplies
the pre-registration payload address, separate from material offset/base
words. Main payload extents total 473,216 bytes; 34 zero-count lightmap
resources retain their words without payload dereference. One empty resource
points to the root boundary. This satisfies source bounds and does not mean
the root contains vertices. Source clears a lightmap's runtime hardware word
when its vertex count is zero; this inspection never mutates raw words.

Recorded shader groups are 11 `senv`, nine `sgla` and 14 `sotr`; all references
are nonnull. The complete numeric reference words are retained privately.
Full datum, primary/ancestor group and bounded-name association against the
raw tag index remain unverified in this gate. Centroid fields contain 27
nonzero uint32 words; these are preserved float bits without arithmetic or
semantic validation. Other fields remain opaque.

The source structures are [structure_bsp_definitions.h](../../../source/structures/structure_bsp_definitions.h),
[rasterizer_geometry.h](../../../source/rasterizer/rasterizer_geometry.h) and
[tag_groups.h](../../../source/tag_files/tag_groups.h). Material stride is
256, lightmap stride 32, material block offset 20. Material field offsets
are shader 0, permutation/flags 16/18, first surface/count 20/24, centroid
28, native buffer metadata 176/196 and tag data 216/236. Buffer metadata and
tag data are each 20 bytes. These disk offsets follow the source's ILP32
declarations and explicit [cache constants](../../../port/linux/game/cache_file_formats.c);
they are not a newly compiled actual-header ABI layout proof.

[Source material validation](../../../port/linux/game/tag_schema_collision.c)
and [resource lookup](../../../port/linux/game/tag_validate.c) set the rules:
signed nonnegative counts, 2,048 materials per lightmap, 64,000 vertices per
material, proper descriptor table membership and bounded payload sizes.
Xbox compressed sizes are 32 and eight bytes; count-derived minimum bytes
must fit the compressed data, allowing extra bytes. The independent resource
checker uses type-indexed sizes 56/32/20/8. The initial metadata helper's
32/8 minimum calculation is scoped to this actual type-1/3 sample, not a
general proof for uncompressed types. PC/CE-only requirements are not applied.
Null hardware-format words are allowed by the source and would provide no
resource-based payload proof; this sample's words are all nonnull.

The selected root/lightmap/material concatenation is 9,416 bytes: root 648,
two lightmaps 64, and 34 materials 8,704. Expected SHA-256 is
`e58ffaeb6e7e82553d1d770a6e21e585d0246f3a0febb3dbba8ec3169e614022`.
This is independent raw-span identity, not executed target serialization,
relocation, a standalone cache, vertex conversion or a durable save.

Independent retained audit passes 831 consistency checks with zero findings,
verifying every material field, signed bound, resource relationship, selected
span and expected concatenation. It adds no extraction, build, launch or SD
write. Earlier campaign audit records and source inputs remain unchanged.

## Candidate and remaining gates

A proposed partial native projection preserves the named numeric fields while
keeping payloads in the existing 22 MiB tag slot. All selected materials must
be projected or rejected atomically; a measured workload count must not become
a source hard cap. Transactional published/scratch workspace, small controls
and selected-span serialization are separately charged. The proposal is not
an implemented decoder or target-measured runtime.

Five strict host/PPC compile/link commands pass with cleared compiler overrides.
The candidate partial material/root/lightmap sizes are 128/24/20 bytes on
both ABIs. Actual selected workspace is two halves of 4,416 bytes, totaling
8,832. Candidate control is 88 bytes on host and 80 on PPC; the caller view
is 56 on both. Host sizes come from execution; PPC sizes come from ELF object
symbols, with no PPC execution. These are candidate layouts, distinct from
the source-derived 256/32-byte disk structures. The view observation is not
a full stack charge or peak measurement.

| Candidate budget | Host allocated span | PPC first placement | PPC alternate placement |
|---|---:|---:|---:|
| Relative required including 2 MiB reserve | 52,582,744 | 52,582,096 | 52,582,096 |
| Placement offset charged to original arena | 0 | 0 | 4,096 |
| Total original arena charge | 52,582,744 | 52,582,096 | 52,586,192 |
| Remaining from 54,368,224 | 1,785,480 | 1,786,128 | 1,782,032 |

The existing planner evaluates 12 slots with 64-byte alignment. Original
tag/state/sound/index/IO reservations and the provisional reserve remain.
The old two 2,048-byte BSP copies are replaced by material workspace and
9,416-byte selected serialization. Existing BSP control and new material
control are charged separately. The earlier UI widget workspace/serialization
are conservatively retained; campaign UI is not qualified. Numeric PPC bases
model alignment/capacity arithmetic without proving accessible target storage;
the alternate placement reduces available capacity by its 4,096-byte offset.
Host alignment remains allocation-dependent. Textures, initialized pools,
geometry conversion, system peak and library/call-chain stack remain excluded.
Larger valid material requests must receive checked storage or reject without
truncation; the 34-material workload does not replace source limits.

Independent proposal audit passes 467 consistency checks with zero findings:
PPC ELF symbols, host observations, compiler/input/artifact identities and all
slot/reserve/placement arithmetic agree. It adds no build or execution. Root
publication validation passes 130 identity, privacy, budget, prefix and link
checks; independent document review reports no findings.

Next implementation should add explicit LE projection and immutable opaque
serialization with parent/BSP epoch checks before workspace access, authored
malformed-input and lifetime coverage, and a fresh raw-tag identity proof
before any actual campaign runtime. Full source reservations remain. Engine
pool initialization, full stack/system peak, shaders/GX rendering, collision,
gameplay and physical Wii remain open. Existing shutdown/ABI failures remain;
HWI-005/006 remain BLOCKED and HWI-007/008 remain TODO.
