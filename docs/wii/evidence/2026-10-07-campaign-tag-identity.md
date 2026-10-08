# Fresh campaign tag identity prerequisite

At clean source `4f3d1d3eec3362d8a4e7713175a4fc4e243d4613`, the existing bounded
tag inspector derived 16,995,988 private tag bytes from the staged fallback map.
All 34 selected material shader references match their raw tag instances by
full salted datum, primary group and `shdr` ancestry. Both reference and instance
names terminate within actual tag bytes. No names or asset bytes are published.
[Numeric evidence](2026-10-07-campaign-tag-identity.json) retains identities and limits.

The tag SHA-256 is `8501f4ad215a630d14220140e27f8a9c30a5ed18fe459ac07c686e827a0ab7bc`,
CRC32 2,283,567,944, with 3,647 instances. Selected scenario datum is 3,782,475,776;
BSP ordinal 12 has full datum 4,021,620,286 and unloaded instance root word zero.
The retained 1,288,192-byte BSP SHA-256 remains
`7f7b4e77ce3667eb0db7bd05e3352330cff47ced9cf3ff7b56a0dc4fc7433e5b`.

Derivation uses `tools/wii/inspect_cache_residency.py` with external new tag/report
outputs. It bounds inflation chunks to 64 KiB and retained tags to 22 MiB,
requires decoder EOF and exact declared 279,763,456-byte map output, and records
the stored map hash. It retains only tags, without another complete map copy.
Observed trailing bytes and CRC identity do not establish canonical Xbox checksum.
The existing BSP inspector then checks the scenario/reference and actual two
readable windows against the fresh tags plus retained BSP. Both commands exit zero.

An independent audit passes 7,797 checks with zero discrepancies. It independently
parses all 3,647 index identities/names/groups, 13 BSP references, selected header,
68 descriptor Data bounds and 34 material shader references. It checks retained
tool/source/record hashes and prior audit preservation. The auditor does not
repeat full map hashing or inflation; EOF and stored-map identity use retained
command evidence. Audit SHA-256 is
`de0a4519e6af3c9b559b7dd7fe3430225e6fc834e478bc7c4dc142a5f7c99d8a`.

Zero name-length words remain valid and unchanged; they are not a positive-length
rule. Equal name addresses are an observation, not a required relationship.
Source derivation retains same-API stat equality booleans, without numeric
timestamp snapshots. Stat guards require immutable inputs and are not locks.
The generic BSP serialization golden covers all 1,288,192 BSP bytes; the earlier
selected material concatenation covers 9,416 bytes. These are different scopes.

This prerequisite qualifies reference metadata. Shader bodies, float semantics,
geometry, native publication/lifetimes, rendering, engine pools, gameplay and
physical Wii remain unqualified. No additional target launch or SD write occurs.
The earlier material proposal remains a historical proposal. HWI-005/006 remain
BLOCKED, HWI-007/008 remain TODO; subsequent authored material implementation has
its own checks and evidence.
