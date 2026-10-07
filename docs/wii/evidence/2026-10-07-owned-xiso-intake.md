# Read-only owned XISO inventory and cache-header identity

Clean source `05b41cf43e50d58f129e61b98357b8f4b42e8f8a` adds a bounded inventory reader
alongside the unchanged runtime maps-only importer. All 45 synthetic tests pass,
covering valid nested trees, four partition locations, malformed descriptors,
cycles, aliases, paths, spans, resource limits, header bounds, immutable reads
and CLI report preservation. Independent source review reconciles header rules
with the existing cache code and finds no unresolved issues.

One owned-image invocation exits 0 in 3.115 seconds. The
full image SHA256 matches the earlier independent file check:
`43ec0813edadd592b7b489a930f108d289db9313ac337611110ffca8be0a7258`; size 3625713664 bytes.
Both volume-descriptor signatures are valid at partition 0 / offset 65536; root
sector 264 / size 88. All reachable trees are traversed with no skipped entries.
This establishes the supplied image's readable filesystem, not canonical dump
authenticity, all unallocated sectors or the filename's stated revision.

| Observation | Result |
| --- | --- |
| Reachable files/directories | 50 / 5 |
| File bytes | 3625127222 |
| Map files / supported headers | 24 / 24 |
| Cache version / build | 5 / 01.10.12.2276 |
| Source region lookup | NTSC |
| Observed scenario values 0/1/2 | 10 / 13 / 1 |
| Movie files | 8: five bink files and three bundled Xbox demo videos |
| maps/ui.map present | Yes |
| Extracted game bytes | 0 |

Header rules require the little-endian numeric head/foot signatures, version 5,
signed declared length/tag bounds and bounded name/build strings. Tag extents
are checked against the declared decompressed length, separately from stored
file size. Unknown build/scenario metadata is not rejected. The Xbox runtime
calls the scenario field reserved; its observed values do not prove campaign or
multiplayer content acceptance. Every stored map length differs from its
declared length. Decompression, checksums, tags, BSPs, geometry, scripts,
conversion and gameplay have not been validated. Neither header acceptance nor
eight movie filenames establishes complete game content or cinematic playback.

```powershell
python tools/wii/xiso_inventory.py '<owned-image>.iso' --output '<external-private-folder>/inventory.json'
python tools/wii/test_xiso_inventory.py
```

Only a new explicitly requested JSON report is written. Image, report, inventory
paths, assets and future extraction stay outside Git; public evidence contains
counts/hashes and safe source/test metadata. Existing output paths are refused.
The reader supports printable ASCII XDVDFS entry names, rejects allocation
aliases, and requires the source to remain immutable during inspection.

The next bounded gate prepares a 32-bit Linux validator and stages private maps
using the inspected existing format/tooling. The existing [map validator](../../../tools/map_validate.c)
uses fixed-address mappings and the project's tag/schema/zlib objects; run
`build/linux/map_validate --strict <private-maps>/*.map` only after a successful
validator build. This is a deeper content gate, still separate from asset
conversion, Wii memory policy, engine runtime and gameplay. HWI-005/006 remain
blocked and HWI-007/008 are not completed by this inventory.

The [sanitized JSON](2026-10-07-owned-xiso-intake.json) records clean tool/test
hashes, input identity, counts, header metadata, test report and explicit limits.
Full inventory and raw commands remain private outside both repositories.
An independent parser, without importing the new implementation, reproduced all
55 entry rows and allocation spans and unpacked all 24 headers/hash values from
67,672 image bytes. It found no discrepancies and performed no extraction or
inflation; the root inventory invocation performed the full-image hash.
