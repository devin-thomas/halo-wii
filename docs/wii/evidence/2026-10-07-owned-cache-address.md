# Owned UI tag index: address/endian and diagnostic residency

The authored index decoder passes on host and emulated PPC with actual owned
UI tag bytes. This is a bounded preparation gate; the engine memory strategy,
production conversion and gameplay remain unqualified.

Source `e7768111dc64c249f90252a43a15a419f37b6943`; companion [JSON](2026-10-07-owned-cache-address.json).
The prior [strict Linux validation](2026-10-07-owned-cache-validation.md) and
all earlier ABI failures/qualification gaps remain unchanged.

## Measured interpretation

All 24 maps were streamed with 64 KiB inflation output chunks, retaining at
most the source's 22 MiB tag-data bound. All staged input hashes and prior
header sizes match. UI contains 1,642,244 tag bytes / 983 instances. Its
36-byte Xbox index header and 32-byte instance records are explicitly decoded
as little-endian words; encoded addresses stay numeric and resolve to checked
offsets in copied storage. Full datums, parent groups and opaque words survive.
Names are bounded NUL spans, roots are one-byte address spans, and unloaded
`sbsp` null roots are explicit. No nested tag/BSP bodies are converted.

Host/PPC each pass 1,916 synthetic cases / 8,437 checks / zero failures or
aborts. Actual UI each passes eight load/use/lossless memory snapshot/unload
cycles, every instance lookup, CRC `e22586e4` and independently computed table
CRC `2e540905`. This is not a durable save format. Naive native first-word
reads differ (`803a6024` host / `24603a80` PPC); explicit decoding agrees.
The actual private source remains unchanged; separate authored synthetic
fixtures exercise mutation/free of the borrowed source after ownership commit.

Four new strict units compile without diagnostics on each target. PPC build
`b74a076b3262eed3`, private DOL SHA256
`8744d307d17d5b4336e8b584c2705463ffd5d52172a07f7244a991c8bb5f5cf7`; no asset-bearing artifact is published.
One owned stock Dolphin 2609 / D3D / existing single-core profile run completes
BEGIN/END result 0 and exits naturally 0 in 5.658 seconds without timeout.
Guest LF SHA256 `dbda48f0df6692ce651318ff98c56ce979926ccf03db72227ad3371c6a5c73b0`.
The settings-file hashes match. This observation does not erase earlier host
shutdown faults or establish general recovery or physical Wii acceptance.

## Capacity and ownership observations

After video/framebuffer/SD setup, this asset-bearing diagnostic measures MEM1
`802e6000..81800000` (22,126,592 bytes) and MEM2 `90002000..933db7e0`
(54,368,224 bytes). These are arena bounds, not total free heap or peak RAM.
The source's fixed Xbox tag reservation `803a6000..819a6000` extends 1,728,512
bytes beyond MEM1 high; current native state at `81a00000` is outside both
arenas. Source macros specify 20 MiB state (the old 16 MB comment is stale),
22 MiB tags, 256 MiB desktop textures and 4 MiB sound: 302 MiB before other
costs. These cannot be copied into Wii as fixed reservations.

Largest declared map is 279,763,456 bytes; largest tag data 16,995,988;
largest BSP 9,039,872. Active BSP metadata reaches the 22 MiB
Xbox tag-window boundary. Whole-map residency is not a viable assumption.
State + tags + sound alone is 46 MiB; subtracting that from this MEM2 arena
leaves 6,133,728 bytes arithmetically, without textures, streaming/IO,
code/stacks, extra system needs, metadata or fragmentation. No allocations
for those full engine pools were attempted, and no capacities were reduced.

Owned graph/index/table-serialization payload sums to 1,705,156 bytes, excluding
embedded source, allocator/code/system/stack overhead. At the sampled live
point heap in-use is 2,488,448; usable payload blocks sum to 1,705,164. Released
heap in-use returns to 783,272, while heap arena size and MEM1 low differ from
startup. A separate MEM2 input copy charges 1,642,272 bytes, resolves/CRC-checks
the index and restores that reservation's low/high bounds. It does not keep
the malloc-owned graph in MEM2 or demonstrate full-program peak/fragmentation.
Compiler static frames are recorded; library/call-chain stack peaks are open.

The inspector reports unrounded BSP lengths and out-of-tag roots rather than
qualifying loader semantics. Trailing compressed-file content is observed,
without canonical padding/checksum validation. Truthful immutable buffers and
no concurrent mutation are preconditions; metadata equality is not writer
exclusion. Header/table proof does not validate full root sizes, nested blocks,
floats, resources, geometry, scripts, conversion, gameplay or upstream peer
capacity compatibility.

The next bounded gate is a measured MEM2 owner/budget and streaming-input
prototype with repeated lifetime/relocation and fragmentation observations.
The selected diagnostic translation is not the engine's final architecture.

Independent review passes 4,287 checks with zero unresolved failures: all 24
prior input identities, actual UI words/names/roots/BSP, host/PPC inputs,
generated private payload, SDK fingerprint, ELF/DOL segments, CRC goldens and
runtime transcript agree. It did not rerun Dolphin or repeat whole-map inflation.
