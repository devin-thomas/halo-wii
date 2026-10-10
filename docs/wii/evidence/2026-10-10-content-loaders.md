# Content loaders, remaining categories and the Research run (HWI-008C)

This evidence covers three things: Wii runtime loaders for the texture,
recorded-animation and sound containers, proven in stock Dolphin; conversion
of models, structure-BSP lightmap geometry, collision, model animations,
fonts and unicode text, with shaders left in place; and the full conversion
on Research compared with Titan. Owned bytes, names, map names and paths
stayed outside Git; this record holds counts, formats and hashes only.
[Numeric evidence](2026-10-10-content-loaders.json) ·
[Pipeline guide](../CONTENT-PIPELINE.md) ·
[Loaders](../../../port/wii/content/README.md)

## Source and identity

| Item | Value |
|---|---|
| Loader commit (DOL) | `1419143f` (clean); the loaders, case tool, runner and input script are unchanged up to `1f9c598c` |
| Converter commit | `1f9c598c` (categories `483154b1` merged with `wii` `60825927`) |
| Image SHA-256 | `43ec0813…0a7258` (the same on Titan and Research) |
| Staging manifest SHA-256 | `acc4e9d1…061ca` |
| Dolphin | stock, `Dolphin.exe` SHA-256 `1ed5e3f6…0dac`, D3D, fresh isolated profile per batch |
| Wii compiler | devkitPPC GCC 16.1.0, C11, `-Wall -Wextra -Werror` |

## 1. Wii runtime loaders

`port/wii/content` reads each container with explicit big-endian field
decoding and loads files from SD into owned, 32-byte-aligned memory:

- **HWT1 textures.** Header, GX format against the profile's source
  mapping, dimensions, faces, levels, image count, reserved bytes and exact
  data length. Every image of every face, level and slice is addressed, and
  a CPU decoder follows the GX rules.
- **HRA1 recorded animations.** The unit-control table of each version,
  every event's type, delta kind, delta, payload widths and unused words,
  one end event at the end, and the delta sum. The Xbox stream is rebuilt
  and must match the recorded source tag.
- **HWS1 sounds.** Codec, rate, channels, reserved bytes, frames and
  payload length; a streaming Xbox ADPCM decoder reads each block header
  explicitly little-endian.

**Cases.** `content_loader_cases.py` sampled the HWI-008B generation
deterministically: 93 cases, 3.47 MB.

| Valid cases | Count |
|---|---|
| Textures: CMPR, RGBA8, RGB565, RGB5A3, IA8 and I8, across 2D, cube and volume | 43 |
| Of which above GX's 1,024 limit (CPU decode only) | 1 |
| Recorded animations (version 4, unit control 0, 3 and 4) | 6 |
| Xbox ADPCM sounds (mono 22 kHz, stereo 22 kHz, stereo 44 kHz) | 6 |
| **Malformed** (textures 14, animations 13, sounds 11) | **38** |

The malformed files cover truncation, trailing bytes, bad magic and
version, unknown enums and profile mismatches, dimensions, counts, lengths,
reserved bytes, unknown and misplaced events, deltas against their kind,
values wider than their field and a changed source tag. Every owned
recorded animation is version 4, so the v1 codec is covered only by the
host fixtures.

**Dolphin run.** Two cold launches of one profile, the host-wide Dolphin
lock held for the batch. Each launch ran every case twice:

| Per cycle | Value |
|---|---|
| Cases passed | 93 of 93 (38 rejected for exactly the stated reason) |
| Texels decoded on the CPU, digest equal to the host's | 1,579,464 |
| GX images read back / 2D mip levels read back through one mip chain | 750 / 130 |
| EFB peeks (colour and alpha passes) | 766,804 |
| Mismatches, maximum error per channel | 0, (0, 0, 0, 0) |
| Wrong-format controls that read back differently | 38 of 42 (the other 4 textures are uniform) |
| Sound frames decoded / animation events rebuilt | 114,432 / 3,681 |
| Cycle CRC | `2c32c90f` in both cycles of both launches |
| Heap growth | 0 |

CMPR interpolated texels were allowed a difference of 8 and showed none:
Dolphin's CMPR blend matches the 5/8 + 3/8 rule exactly.

| Outcome | Run 1 | Run 2 |
|---|---|---|
| Guest | passed | passed |
| Persistence | passed (counter 1, staged files intact) | passed (counter 2, prior log preserved) |
| Host process | exit 0, 10.1 s | exit 0, 9.4 s |
| OS stability | no instability events in the window | |
| Physical Wii | untested | |

Emulated time for the valid cases of one cycle: textures 3.11 MB loaded in
93 ms and decoded in 653 ms; sounds 117 KB in 14 ms and 46 ms; animations
60 KB in 14 ms and 2 ms. These are Dolphin's time base, not hardware.

**Host build.** `test_content_loaders.py` compiles the same loaders with the
host compiler and runs the case list built from authored fixtures (all six
GX formats, cube and volume, both animation codecs, mono and stereo ADPCM,
every malformed variant) plus a changed texel and a changed nibble, which
must fail.

## 2. Remaining categories

| Category | Decision | Files | Bytes | Contents |
|---|---|---|---|---|
| Models | HWM1 | 1,481 | 85,246,848 | 4,644 geometries, 10,862 parts, 2,386,375 vertices, 4,045,905 indices |
| BSP lightmaps | HWL1, one per structure BSP | 82 | 146,230,304 | 2,288 lightmaps, 14,642 materials, 3,329,120 + 3,103,411 vertices, 2,012,273 surfaces |
| Collision | HWC1 | 936 (854 models, 82 BSPs) | 146,645,184 | 3,814 BSPs, 9,955,825 elements |
| Model animations | HMA1 | 628 | 70,005,984 | 12,711 animations (186 compressed), 468,552 frames |
| Fonts | HWF1 | 96 | 3,679,488 | 21,504 characters, 2,790,576 glyph pixel bytes |
| Unicode text | HUS1 | 452 (418 lists, 34 HUD texts) | 495,200 | 5,431 strings, 204,713 code units |
| Shaders | in place | 0 | 0 | 4,542 tags checked |
| **New total** | | **3,675** | **452,303,008** | |

- **Conversion.** Every record is decoded with an explicit little-endian
  field format and re-encoded big-endian; floats keep their bits. Each
  output must rebuild its exact source bytes, and the converter's reader
  re-validates every published container.
- **Lightmap bitmaps** were already converted with the textures; HWL1 names
  its lightmap bitmap tag.
- **Shaders.** No shader group has a tag-data or file-offset field in the
  upstream validator schema (a test walks every shader definition), and the
  Xbox pixel programs are in the executable. The parameters are typed tag
  fields that the engine reads in place after ADR-015 relocation. The
  census checked every root lies in tag data: senv 496, soso 2,498,
  schi 502, sgla 163, smet 129, sotr 707, spla 39, swat 8.
- **Owned layout facts.** Every model part uses compressed vertices and
  precompiled strips. Every BSP material uses compressed environment and
  lightmap vertices whose data size matches its counts. All 186 compressed
  animation blocks are covered exactly by their arrays. Two rules were
  relaxed after the first owned run, each by its evidence: blank glyphs
  carry a negative width with height 0, and empty collision leaves name
  first reference -1.
- **Fixture tests.** `test_content_categories.py` authors every category in
  a synthetic map. It checks the decoded values, determinism, nine defects
  (each rejected with `CURRENT` and the tree unchanged), interruption and
  disk-full, container validation, and every tag offset against
  `cache_schema_tables.c`.

## 3. Full conversion: Research and Titan

All ten categories, all 24 maps, fresh output roots:

| Host | Run | Exit | Converter time |
|---|---|---|---|
| Research (Apple M5 Max, 18 cores, macOS 27, Python 3.12.15) | R1 | 0 | 272.3 s |
| Research | R2 | 0 | 272.9 s |
| Titan (Windows 11, Python 3.14.0, shared with other agents) | T1 | 0 | 1,431.9 s |

The converter is a single process; HWI-008B measured 1,171.6 s and
1,563.0 s on Titan for the first three categories alone.

- **Generation** `17189aa32aa83526` (manifest `17189aa3…e6b1`) on all three
  runs: 91,397 outputs, 91,398 files with the manifest, 3,689,821,826 bytes.
- **Tree digest** `afb8532b…8166` for R1, R2 and T1: byte-identical across
  runs and hosts.
- **Unchanged categories.** All 87,722 texture, recorded-animation and sound
  outputs have the same path, size and SHA-256 as HWI-008B's generation
  `01bf0850…`.
- **Research input.** The 24 maps were copied out of Research's own copy of
  the image (same SHA-256), and each matched the staging hash first.

**Safety on owned data (Research).** New categories for two maps were
converted into the root whose `CURRENT` was the full generation:

| Case | Bytes written | Exit | `CURRENT` unchanged | Staging left |
|---|---|---|---|---|
| Disk full at 0 bytes | 7,872 | 4 | yes | 0 |
| Disk full at 20 MB | 20,039,424 | 4 | yes | 0 |
| Stop before validation | 57,335,571 | 4 | yes | 0 |
| Stop before promotion | 57,335,571 | 4 | yes | 0 |
| Stop before the `CURRENT` update | 57,335,571 | 4 | yes | 0 |
| Killed (SIGKILL) after 1 s | - | -9 | yes | 1 |
| Next run after the kill | - | 0 | (published) | 0 |

A first kill at 4 s missed: that run reused the generation the
stop-before-`CURRENT` case had renamed and finished in under 4 s. The kill
was repeated on maps not converted before. The full generation was then
re-verified, reused and made current again, and its tree digest was
unchanged. Research scratch outputs were deleted afterwards.

**Tests.** The `tools/wii` suite passes on Python 3.14 and 3.11. The
build-only artifact staging and inspection pass with the new target.

## Not established

- Physical Wii, real SD and FAT behaviour, and hardware decode cost.
- Wii loaders for HWM1, HWL1, HWC1, HMA1, HWF1 and HUS1; only Python
  decoders exist. Their consumers are HWI-015B, HWI-016 and HWI-032.
- Byte order of the remaining tag fields read in place (shaders and the
  rest of the tag cache). The validator schema does not type scalars.
- Scripts and movies; texture and sound residency (HWI-007).
- GX handling of cube maps, volumes and the oversize textures.

HWI-008 stays open (see the JSON `acceptance` block).
