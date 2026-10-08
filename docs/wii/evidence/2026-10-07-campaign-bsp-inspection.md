# Bounded campaign BSP inspection

Read-only inspection at clean source `e19b2de2bfff47816a5950167d75b8f90a74a655`
finds a campaign BSP with nonempty vertex/index descriptor tables and five
nonempty selected root blocks. This follows the
[UI BSP diagnostic](2026-10-07-owned-bsp-residency.md).
[Numeric evidence](2026-10-07-campaign-bsp-inspection.json) contains identities,
counts and bounds without asset bytes, names or private paths. No target run,
SD write, native projection or geometry conversion occurs in this gate.

## Actual samples and source limits

The smallest campaign candidate, map ordinal 14 / BSP 9, is 8,192 bytes at
logical map offset 53,688,320, encoded base `819a4000`. Actual descriptor
counts are zero/zero, despite separate tag-header metadata counts 558/558.
Its root is at BSP offset 24; collision BSP count is one, lightmaps two and
clusters one. Collision materials and surfaces are empty. The 8,192-byte
sample SHA-256 is
`fa15c57a0f556df8240abc4a12f38440729d0bec29c1e5f9a0dfe44d30a1b78d`.

Because that descriptor workload is empty, the authorized fallback inspects
map ordinal 4 / BSP 12: 1,288,192 bytes at logical offset 40,673,280, base
`8186b800`. Its actual descriptor counts are 34/34, distinct from tag-header
metadata 722/722. Tables occupy BSP offsets 24 and 432, 408 bytes each.
All 68 descriptor Data addresses fit the BSP for one byte; no full payload
length or vertex-format proof follows. One address equals the root start;
this is allowed because Data spans are bounded, not claimed as disjoint from
the root. The 12-byte D3D resource descriptors are distinct from native
vertex-buffer structures; opaque Common/Lock words are not vertex counts.
Fallback SHA-256 is
`7f7b4e77ce3667eb0db7bd05e3352330cff47ced9cf3ff7b56a0dc4fc7433e5b`;
CRC is `adf28ea7`. Root offset is 474,080, extent 648.

| Selected root block | Root field offset | Stride | Source maximum | Actual count | BSP span offset / bytes |
|---|---:|---:|---:|---:|---:|
| Collision materials | 164 | 20 | 512 | 17 | 474,728 / 340 |
| Collision BSP | 176 | 96 | 1 | 1 | 475,068 / 96 |
| Surfaces | 248 | 6 | 131,072 | 8,084 | 1,180,616 / 48,504 |
| Lightmaps | 260 | 32 | 128 | 2 | 1,229,120 / 64 |
| Clusters | 308 | 104 | 512 | 11 | 1,238,000 / 1,144 |

Signed counts, source limits, count-times-stride and selected extents pass.
The declarations are [structure_bsp_definitions.h](../../../source/structures/structure_bsp_definitions.h),
limits [structures.h](../../../source/structures/structures.h) and
[tag_schema_collision.c](../../../port/linux/game/tag_schema_collision.c).
Root size, lightmap offset/stride and nested material offset/stride are explicit
in [cache_file_formats.c](../../../port/linux/game/cache_file_formats.c).
Other root offsets are derived from ILP32 declarations, without a new compiled
offsetof proof. Source validator maxima are not independently qualified Xbox
hard caps. The PC/CE uncompressed-geometry verifier is not applied to Xbox.

## Bounded derivation and limitations

Both stored-map hashes match the existing immutable inspection metadata.
Inflation uses output chunks bounded to 65,536 bytes, discarding earlier
logical bytes and retaining only the selected BSP. The first sample produces
53,694,464 prefix bytes while retaining 8,192; the fallback produces 41,959,424
while retaining 1,288,192. These prefix counts exclude the 2,048-byte cache
header; logical map offsets include it. Neither reaches decoder EOF or newly revalidates
the complete stream/canonical checksum. The source image is untouched.
File-handle/path identity and metadata remain unchanged; read-only guards
do not constitute locks or exclude transient writers.

Initial execution fails before writing any sample because Windows fstat and
Path.stat expose different ctime values. Device/inode/size/mtime agree. The
failed helper and record remain private. Corrected checks compare ctime only
across observations from the same API, retain cross-API stable fields and
verify the full stored-map hash. Independent isolated comparison reproduces
the API mismatch; its exact helper attempt stops earlier at the exclusive
existing-output guard and does not reproduce the historical assertion.

The selected association comes from previously verified scenario metadata;
this gate does not newly decode the complete raw tag index/full datum. Root
block contents, nested references, vertex counts/formats, collision topology,
geometry payload sizes and endian conversion remain unqualified.

Independent retained audit passes 217 consistency checks with zero findings:
sample SHA/CRC, headers, root/table extents, all 68 descriptor Data bounds,
five selected root blocks, prior metadata and source identities agree. It
confirms that the root-boundary Data address satisfies the current source
rule. The audit adds no inflation, target run or SD write. Root publication
validation passes 49 identity/privacy/budget/link/prefix checks; independent
document review confirms the stated scope.

Two complete fallback stage/serialized copies require 2,576,384 bytes.
Replacing today's 4,096 bytes still adds 2,572,288, exceeding the measured
1,796,360-byte PPC remainder by 775,928. The next candidate must stream into
the existing 22 MiB tag slot and use bounded projection/chunked serialization,
without reducing source capacities. That candidate is not target-measured.

Next work is bounded named root -> lightmap -> material metadata inspection,
using material stride 256 and preserving reference identities, counts, numeric
bits and opaque fields before selecting native workspace. Host shutdown,
engine strategy, gameplay and physical Wii remain open. HWI-005/006 remain
BLOCKED and HWI-007/008 remain TODO.
