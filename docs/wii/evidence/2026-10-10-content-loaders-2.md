# Wii loaders for models, lightmap geometry, collision, model animations, fonts and text (HWI-008E)

This evidence covers the C runtime loaders for the six sectioned containers
HWI-008C added (HWM1, HWL1, HWC1, HMA1, HWF1, HUS1), the renderer and
engine decisions they make, a host sweep of every converted file through the
same C code, and two cold launches of the loader self-test in stock Dolphin
on two hosts. Owned bytes, names, map names and paths stayed outside Git;
this record holds counts, formats, sizes and hashes only.
[Numeric evidence](2026-10-10-content-loaders-2.json) ·
[Loaders](../../../port/wii/content/README.md) ·
[Pipeline guide](../CONTENT-PIPELINE.md) ·
[HWI-008C loaders](2026-10-10-content-loaders.md)

## Source and identity

| Item | Value |
|---|---|
| Loader commit (DOL) | `465c9ac8` (clean) |
| Content generation | `17189aa32aa83526` (HWI-008C full conversion, unchanged) |
| DOL | `content_loaders.dol`, 548,320 bytes, SHA-256 `4bdf6c72…f2e2`, build id `892c076edf870298` |
| Wii compiler | devkitPPC GCC 16.1.0, C11, `-Wall -Wextra -Werror -ffp-contract=off` |
| Host compiler | MinGW-w64 GCC 13.1.0, the same warnings |

## 1. Loaders

Each loader validates the 32-byte header (magic, version, header size,
flags, reserved bytes, total length), the section table (count, ids,
record sizes, offsets, sizes, zero padding, nothing after the last section)
and every record, then decodes into one owned 32-byte aligned arena. The
file buffer can be released at once; the self-test releases it before the
digest, so each digest covers the decoded form alone. Fields are assembled
with shifts from big-endian bytes and stored as native values; decoded
structs are checked against their record formats with static assertions
(ADR-018). Each loader exposes a canonical digest: SHA-256 of every decoded
field written little-endian in a documented order, which
`tools/wii/content_decoded.py` computes independently.

| Loader | Checks beyond the container | Decoded form |
|---|---|---|
| HWM1 models | vertex type 5, strip type 0/1, counts, geometry/part order, contiguous ranges tiling the arrays, every index below its part's vertex count | parts; GX indexed vertices of 56 bytes; u16 indices |
| HWL1 lightmap geometry | BSP record against the sections, lightmap material ranges, vertex types 1/3, counts, ranges, surfaces inside the array, local surface indices below the material's vertex count | lightmaps, materials; environment vertices of 56 bytes; lightmap vertices of 16 bytes; u16 surfaces |
| HWC1 collision | per-BSP ranges and the engine's maxima; every index the engine follows: 3D node planes and children (node, NONE or leaf by sign bit), leaf references, 2D reference planes (flip bit) and nodes, 2D node children (node or surface by sign bit), surface planes and first edges, edge vertices, edges and surfaces (-1 none), vertex edges | the engine's eight record arrays |
| HMA1 model animations | index, pad, node count, frame info type, frame size against the node flags, the four data ranges in order and tiling their sections, frame info, default and frame sizes, and for compressed blocks the header, node headers and arrays tiling the block exactly | the Xbox frame info, defaults, frames and compressed blocks in native order |
| HWF1 fonts | font record against the sections, at most 256 tables of at most 256 entries tiling the indices, indices -1 or a character, glyph pixels inside the pixel data | records, tables, indices; 8-bit glyph coverage verbatim; the engine's character lookup |
| HUS1 text | string count and lengths, ranges tiling the code units | native UTF-16 code units |

The HWC1 index rules come from the engine's traversal code and were
confirmed on the owned data before they were enforced: across all 936
collision files no index breaks them (NONE children appear only in 3D
nodes; flipped planes appear in 2D references and surfaces).

### Decisions

| Question | Decision | Why, and what it costs |
|---|---|---|
| Packed 11:11:10 normal, binormal, tangent, incident radiosity | Expand at load to F32 with the engine's `uncompress_int32_to_real_vector3d`, bit for bit (GX_NRM_NBT F32) | GX has no 11:11:10 format, and S16/S8 normals are not exact. 36 bytes per vertex instead of 12 |
| Model and lightmap texcoords (s16) | Keep s16 (GX_TEX_ST S16); the engine's `(2s + 1) / 65535` is affine and belongs in the texture matrix | Exact data, no extra bytes |
| Index width | 16-bit (GX_INDEX16) | Every part and material has at most 65,535 vertices (largest part on the owned data: 5,628); GX has no 32-bit indices |
| Primitive type | Model parts as stored precompiled strips (GX_TRIANGLESTRIP draws their degenerate joins as the Xbox does); BSP surfaces as triangle lists local to their material | No re-stripping, data stays exact |
| Two-node skinning | Left to the renderer (HWI-032); node bytes and weights stay as stored | GX takes one matrix index per vertex. On the owned data 2,240,801 of 2,386,375 model vertices (94%) have node bytes that are not multiples of 3, so the engine's `/3` decode does not fit them |
| Compressed model animations | Keep compressed in memory, decode at playback with the engine's keyframe code | 186 compressed animations hold 1,767,250 bytes; expanded at load they would need 4,437,112 bytes (2.5x). 279 arrays inside the blocks sit at 2-byte offsets, so playback must read them alignment-safely |
| Glyphs | Keep 8-bit linear coverage; the text renderer fills a GX I8/I4 cache texture with the glyphs it draws | The font is never tiled; every font has 224 characters |

## 2. Host results

**Fixture tests.** `test_content_loaders.py` builds the loaders with the
host compiler and runs every case staged from an authored map with all
categories (both maps carry every category): every valid file decodes to
the host digest, every malformed variant rejects for its stated reason
(all 14 error classes appear), the three wrong-format controls reject, a
flipped packed-normal bit is still a valid file but fails its digest,
packed-vector extremes match the host decoder, and a sweep of every
decoded-kind output passes. The authored collision BSP now uses the
engine's child and plane encodings.

**Full sweep on the owned generation.** `content_loader_cases.py --sweep`
listed every output of the six kinds, and the host build of the committed
loaders decoded each one to the digest the independent Python decoder
computed:

| Kind | Files | Passed | File bytes | Decoded bytes | Largest decoded |
|---|---|---|---|---|---|
| Models | 1,481 | 1,481 | 85,246,848 | 142,393,696 | 916,160 |
| Lightmap geometry | 82 | 82 | 146,230,304 | 250,944,160 | 7,382,560 |
| Collision | 936 | 936 | 146,645,184 | 146,465,472 | 3,449,248 |
| Model animations | 628 | 628 | 70,005,984 | 70,332,480 | 934,368 |
| Fonts | 96 | 96 | 3,679,488 | 3,664,128 | 57,152 |
| Text | 452 | 452 | 495,200 | 466,272 | 6,272 |
| **Total** | **3,675** | **3,675** | 452,303,008 | 614,266,208 | |

The decoded data of all 628 model animation files, 96 fonts and 452 text
files (1,176) also rebuilds the converter's source hash of the original
Xbox bytes. Models and lightmap geometry grow 1.67x and 1.72x from the F32
vector expansion; the other kinds stay within 1%.

## 3. Loader cases

`content_loader_cases.py` sampled generation `17189aa32aa83526`: 210 cases,
17,990,198 bytes. The texture, recorded-animation and sound cases are the
HWI-008C set (the outputs are unchanged).

| Kind | Valid | Of which | Malformed |
|---|---|---|---|
| Textures, recorded animations, sounds | 43 / 6 / 6 | the HWI-008C set | 14 / 13 / 11 |
| Models | 3 | smallest, median, largest (85 geometries, 155 parts, 15,193 vertices) | 15 |
| Lightmap geometry | 3 | smallest, median, largest BSP (317 materials, 96,903 vertices, 57,532 surfaces) | 15 |
| Collision | 4 | smallest, median, largest collision model (6,309 elements), largest structure BSP (233,508 elements) | 15 |
| Model animations | 5 | smallest, median, largest (176 animations), smallest compressed (3), largest and most compressed (22) | 17 |
| Fonts | 4 | smallest, median, largest (48,048 glyph pixel bytes); all fonts have 224 characters | 15 |
| Text | 5 | smallest, median, a HUD text, most strings (46), longest list (2,963 code units) | 13 |
| Wrong-format controls | | a model given to the lightmap loader, a collision file relabelled HWL1, a font given to the text loader | 3 |
| **Total** | **79** | | **131** |

Malformed variants per sectioned format: header truncated, trailing bytes,
bad magic, version, flags, total length, section count, record size,
section offset and section padding, plus per-format defects: vertex and
strip types, part order, vertex range gap, index past vertices (models);
BSP counts, lightmap material gap, material vertex type, surfaces outside,
surface index past (lightmaps); BSP range gap, node child, leaf reference,
surface plane and edge vertex outside (collision); index, pad, node count,
frame info type, frame size, frame info offset, a moved compressed array
(model animations); font counts, table gap, table too long, index outside,
glyph outside (fonts); string gap, units short, string too long (text). Each
is expected to reject for one stated reason: truncated, length, magic,
version, reserved, section, enum, count or range.

## 4. Stock Dolphin

The DOL from the clean commit `465c9ac8` (build `892c076edf870298`) ran two
cold launches of one fresh profile per host, each launch running every case
twice, with the host's Dolphin lock held for the batch.

| Host | Dolphin | Backend | Launch 1 | Launch 2 | Batch wall time |
|---|---|---|---|---|---|
| Research (Apple M5 Max, macOS 27.0) | 2609, `Dolphin` SHA-256 `b3c369bc…3ccf` | Vulkan | 15.9 s | 15.8 s | 44.0 s |
| Titan (Windows 11 10.0.26200) | stock, `Dolphin.exe` SHA-256 `1ed5e3f6…0dac` | D3D | 19.5 s | 19.2 s | 198.8 s (SD staging through WSL mtools) |

| Per cycle, both hosts, both launches | Value |
|---|---|
| Cases passed | 210 of 210 (131 rejected for exactly the stated reason) |
| By kind | textures 57, recorded animations 19, sounds 17, models 18, lightmap geometry 20, collision 19, model animations 22, fonts 19, text 19 |
| GX images / mip levels read back, mismatches | 750 / 130, 0 (max channel error 0) |
| Texture wrong-format controls detected | 38 of 42 |
| Cycle CRC | `44383094` in both cycles of both launches on both hosts |
| Heap growth | 0 |

| Outcome | Research | Titan |
|---|---|---|
| Guest | passed, both launches | passed, both launches |
| Persistence | passed (run counter 1 then 2, prior log kept, staged files intact) | passed (the same) |
| Host process | exit 0 both launches | exit 0 both launches |
| OS stability | no instability events in the batch window | no instability events in the batch window |
| Physical Wii | untested | untested |

A first Research attempt did not start: the lock was free, but another
agent's Dolphin was still running, and the runner refused to start without
exclusive ownership. It was repeated once the host was idle. A development
run of a dirty tree on Titan earlier gave the same 210/210 and CRC.

## 5. Memory and load time per asset (guest)

Emulated time (Dolphin's time base, not hardware), identical on both hosts.
Load is the SD read into owned memory; decode is validation plus decode into
the arena; the digest is test-only. Heap peak holds the file and the decoded
form; resident is the decoded form alone after the file is released (heap
bytes above the case's start, allocator overhead included).

| Largest asset per kind | File bytes | Decoded bytes | Heap peak | Heap resident | Load | Decode |
|---|---|---|---|---|---|---|
| Lightmap geometry (96,903 vertices, 57,532 surfaces) | 4,281,792 | 7,382,560 | 11,664,368 | 7,382,568 | 46.4 ms | 42.3 ms |
| Collision, structure BSP (233,508 elements) | 3,449,440 | 3,449,248 | 6,898,704 | 3,449,256 | 38.4 ms | 34.0 ms |
| Model animations (176 animations) | 928,864 | 931,552 | 1,860,432 | 931,560 | 12.8 ms | 5.8 ms |
| Model (15,193 vertices, 28,309 indices) | 551,616 | 916,160 | 1,467,792 | 916,168 | 7.9 ms | 5.4 ms |
| Font (224 characters, 48,048 pixel bytes) | 57,312 | 57,152 | 114,480 | 57,160 | 3.7 ms | 0.2 ms |
| Text (39 strings, 2,963 code units) | 6,336 | 6,272 | 12,624 | 6,280 | 2.8 ms | 0.1 ms |

Other sampled assets: the largest collision model (92,544 bytes) loads in
3.8 ms and decodes in 0.9 ms; the most compressed graph (22 compressed
animations, 310,144 bytes) holds 310,400 bytes decoded and would need
724,324 more bytes of frames if its animations were expanded at load. Per
cycle, the 24 valid sectioned-container cases (11.6 MB of files, 16.4 MB
decoded) loaded in 183.5 ms and decoded in 109.4 ms.

## 6. Build and tests

| Step (Titan) | Result | Time |
|---|---|---|
| `configure.py --wii` and every Wii target at `465c9ac8` | built | 177.8 s |
| `package_artifacts.py stage` and `inspect --require-clean` | 29 allowlisted files, 0 problems, 0 findings | |
| `tools/wii` suite, Python 3.14 and 3.11 | 394 tests OK each | 16.3 s, 16.9 s |
| Case staging (210 cases) | written | 10.3 s |

## Not established

- Physical Wii, real SD and FAT behaviour, and hardware load and decode
  cost.
- Drawing the decoded geometry: the vertex layouts are chosen for GX
  indexed arrays but not drawn here; skinning, the texcoord matrices,
  compressed-animation playback and the glyph cache belong to their
  consumers (HWI-015B, HWI-016, HWI-032).
- Whole-map residency: these are per-asset numbers for HWI-007 and
  HWI-032, not a map budget.
- Byte order of the tag fields read in place, scripts and movies.

HWI-008 stays open (see the JSON `acceptance` block).
