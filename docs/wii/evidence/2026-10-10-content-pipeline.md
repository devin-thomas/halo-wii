# Host content inventory and conversion baseline (HWI-008)

This evidence covers a host-only Python pipeline run on the user's owned Halo
dump: an inventory of the content, GX texture conversion, explicit decoding
of recorded animations, sound containers, and atomic publication. All of it
ran on a Windows host. No Wii, Dolphin or runtime loader was involved, and
nothing here is a target or hardware claim. Owned bytes, names, paths and the
converted outputs stayed outside Git. The tables give counts, formats and
hashes only. [Numeric evidence](2026-10-10-content-pipeline.json) ·
[Running the pipeline](../CONTENT-PIPELINE.md)

## Source and identity

| Item | Value |
|---|---|
| Source commit (clean `tools/`) | `4062b2d6a5e4ac17f461f6e795bb284b7b6d4897` |
| Python | 3.14.0 (Windows); tests also on 3.11.4 (Windows) and 3.13.5 (Debian WSL) |
| Image SHA-256 | `43ec0813edadd592b7b489a930f108d289db9313ac337611110ffca8be0a7258` |
| Staging manifest SHA-256 | `acc4e9d1ab2da3d659c0508b67358d9b742bc924bd4754f28c5e0af4ad0061ca` |
| Maps | 24 staged maps, each hash-checked against staging while being read |
| Source immutability | Image, `staging.json` and all 24 maps hash-identical before and after every run |

All runs used `4062b2d6`. The commit carrying this evidence also removes
one unused import from `content_inventory.py` and two from a test. Those
modules are outside the converter's module hashes, so no behaviour changed.

The maps are the private staging created by the earlier intake, whose image
and per-map hashes were verified then. Nothing was extracted again.

## 1. Canonical inventory

`content_inventory.py` was run twice and produced byte-identical private and
public reports (public SHA-256 `8d5e0586…ed25`, about 19.5 s each). Maps are
named by category and ordinal only. The full per-tag report, with tag names,
is private. It is the expected set for future completeness tests.

| Category | Maps | Recorded animations | Bitmaps | Sound tags |
|---|---|---|---|---|
| Campaign | 10 | 349 | 8,118 | 11,559 |
| Multiplayer | 13 | 0 | 6,313 | 4,110 |
| UI | 1 | 0 | 384 | 29 |
| **Total** | **24** | **349** | **14,815** | **15,698** |

- **Tag index.** 70 tag groups and 54,376 tag instances over the 24 maps,
  including 9,327 bitmap groups, 15,698 sound and 734 looping-sound tags,
  628 model-animation tags, 82 structure BSPs and 24 scenarios.
- **Bitmaps by type.** 14,100 2D, 572 cube maps and 143 volumes. Every
  dimension is a power of two. 24 bitmaps are linear (pitched rows). Every
  non-linear uncompressed bitmap carries the swizzled flag, and no compressed
  bitmap does.
- **Sounds.** All 72,558 permutations use Xbox ADPCM (format counts below).
  There is no PCM, IMA or Ogg. The total is 1,606,686,372 bytes, about
  82,985 s of audio, which would be 5,712,662,656 bytes as 16-bit PCM.
- **Recorded animations.** All 349 are version 4. By unit-control version:
  271 use v4, 71 v3 and 7 v0. They hold 352,843 stream bytes and 90,489
  events. For every one, `length_in_ticks` equals the sum of event deltas
  plus 1.
- **Text.**
  - 96 font tags.
  - 418 unicode string-list tags holding 5,397 strings (383,734 bytes).
  - 24 string-list tags holding 360 strings.
  - 34 HUD message-text tags.
- **Disc files besides maps (26).**
  - The game executable.
  - Five Bink movies for the game.
  - A demo launcher: its executable, three Bink videos, one WMA
    stream, two PCM WAVs, nine PNGs, two XPR font packs and two small
    unidentified files.

| Bitmap format | Count | Source bytes | GX bytes (`gx-baseline-v1`) |
|---|---|---|---|
| dxt1 → CMPR | 5,668 | 232,593,152 | 232,243,680 |
| dxt3 → RGBA8 | 2,816 | 180,569,344 | 721,987,392 |
| dxt5 → RGBA8 | 157 | 31,495,808 | 125,964,736 |
| r5g6b5 → RGB565 | 2,328 | 130,953,728 | 130,939,200 |
| ay8 → I8 | 1,293 | 9,129,728 | 9,148,928 |
| x8r8g8b8 → RGBA8 | 879 | 15,202,048 | 15,308,416 |
| a8y8 → IA8 | 639 | 33,962,112 | 33,961,408 |
| a8r8g8b8 → RGBA8 | 446 | 31,803,648 | 31,827,776 |
| p8_bump → RGBA8 | 305 | 61,530,880 | 246,107,072 |
| a1r5g5b5 → RGB5A3 | 173 | 29,334,016 | 29,334,016 |
| y8 → IA8 | 78 | 4,278,912 | 8,551,712 |
| a4r4g4b4 → RGB5A3 | 26 | 262,144 | 262,144 |
| a8 → IA8 | 7 | 611,968 | 1,223,712 |
| **Total** | **14,815** | **761,727,488** | **1,586,860,192** |

GX byte counts include tile padding. Six bitmaps exceed GX's 1,024-texel
limit; they are converted unchanged and flagged.

| Sound codec | Permutations | Bytes | Seconds |
|---|---|---|---|
| Xbox ADPCM, mono, 22,050 Hz | 59,681 | 821,420,172 | 66,227 |
| Xbox ADPCM, stereo, 22,050 Hz | 1,151 | 46,179,504 | 1,861 |
| Xbox ADPCM, stereo, 44,100 Hz | 11,726 | 739,086,696 | 14,897 |

**Finding: bitmap pixel offsets.** In every bitmap group the pixel-data tag
field has size 0 and a nonzero file offset. The January source
`source/cache/xbox_texture_cache.c` adds that offset to each bitmap's own
offset. Two layouts were tested:

| Pixel addressing | Overlapping spans across all maps |
|---|---|
| Each bitmap's own offset | 0 (the spans tile the pixel region) |
| Own offset plus the group's file offset | 5,857 |

The converter uses each bitmap's own offset. A Wii texture loader must not
copy that addition without its own evidence.

## 2. Conversion (`gx-baseline-v1`)

`content_convert.py` converted all 24 maps and all three categories:

| Kind | Files | Bytes |
|---|---|---|
| Textures (HWT1) | 14,815 | 1,587,808,352 |
| Sounds (HWS1) | 72,558 | 1,609,008,228 |
| Recorded animations (HRA1) | 349 | 1,493,524 |

- **Run.** Exit 0 after 1,171 s. Every planned item was validated first;
  there were no rejections.
- **Generation.** `01bf08505713bcb9`, manifest SHA-256 `01bf0850…`
  (full value in the JSON). The manifest is 37.5 MB and lists 87,722 outputs.
- **Size limits.** The largest file is 2,796,288 bytes. There are 58 output
  directories, and the largest holds 7,816 files, about 15,632 FAT directory
  entries with long names, under FAT32's 65,534 per directory.

**Determinism.** A second full run into a fresh output root (exit 0,
1,563 s) produced the same manifest SHA-256. All 87,723 files, the
manifest included, were byte-identical (tree digest `d1ea297e…26db`).
A third run into the first root (2,302 s, mostly re-verification) found the
existing generation, verified it and reused it. That tree was still
identical afterwards.

**Texture verification.** During the run, 2 items of each format and type
per map were checked:

- an independent GX decoder read back every texel of every face, level and
  slice (42.0 million texels, 838 textures);
- each texel was compared with a reference decode of the Xbox source;
- no texel exceeded its tolerance.

| Source format and type | Textures | Texels | Exact | Max error (R, G, B, A) |
|---|---|---|---|---|
| dxt1 2D / cube | 48 / 48 | 3,703,920 / 1,571,328 | 67% / 61% | 11,11,11,0 / 10,10,11,0 |
| dxt3 2D | 48 | 5,878,228 | 100% | 0 |
| dxt5 2D | 48 | 3,992,020 | 100% | 0 |
| p8_bump 2D | 46 | 7,361,870 | 100% | 0 |
| a4r4g4b4 2D | 25 | 114,688 | 87.9% | 0,0,0,17 |
| a1r5g5b5 2D | 47 | 3,639,552 | 100% | 0 |
| r5g6b5 2D / cube | 46 / 46 | 2,837,153 / 1,998,756 | 100% | 0 |
| a8r8g8b8 2D / cube | 48 / 24 | 1,372,827 / 196,560 | 100% | 0 |
| x8r8g8b8 2D / cube / 3D | 48 / 48 / 48 | 356,336 / 952,608 / 3,504 | 100% | 0 |
| a8y8 2D / 3D | 48 / 23 | 1,235,968 / 861,327 | 100% | 0 |
| ay8 2D / 3D | 48 / 24 | 196,608 / 898,776 | 100% | 0 |
| y8 2D / cube | 47 / 23 | 1,212,401 / 3,014,610 | 100% | 0 |
| a8 2D | 7 | 611,667 | 100% | 0 |

- **DXT1 differences.** They are only the documented GX interpolation (5/8 +
  3/8 against 2/3 + 1/3). The worst error, 11, equals the bound
  `|c0−c1|/24+1`.
- **a4r4g4b4.** The only difference is alpha quantization, at most 17.
- **GX tile order.** The authored tests separately reproduce the
  `port/wii/gx_materials` tile order and its DXT1-to-CMPR converter byte for
  byte. That converter is the one Dolphin verified in HWI-014.

**Recorded animations (ADR-018 obligation).**

- All 349 streams decode with explicit little-endian field reads and
  shift/mask header decoding.
- Each one rebuilds its exact source bytes, both from the decoded values and
  from the canonical big-endian HRA1 record.
- There is no trailing data after any end event.

The tests cover the PPC bit-field case: header byte `0x0D` decodes as kind 1,
type 3. Signed char deltas decode negative whatever the host's `char`.

**Sounds.** The ADPCM payloads are kept verbatim, with explicit big-endian
headers. 144 sampled permutations decoded their first 64 blocks without
error.

## 3. Safety

**Authored tests** (42 new; 321 in the whole `tools/wii` suite):

- **Python versions.** The suite passes on Windows Python 3.14.0 and 3.11.4.
  The new tests also pass on Debian WSL Python 3.13.5.
- **Inflate rejections:** bad head or foot, unsupported version, declared
  length above the bound or differing from the stream, tag extent outside
  the map, truncated header, truncated stream and corrupt stream.
- **Tag index rejections:** bad signature, instance count 0 or above 65,535,
  instance table below the base, datum/ordinal mismatch and a wrong scenario
  datum.
- **Item rejections.** Each of these rejects the whole run:
  - a mip count above the chain;
  - pixels past the end of the map;
  - a negative block count;
  - an animation stream outside tag data;
  - a partial ADPCM block;
  - mismatched compression;
  - every truncation of both animation codecs, unknown event types and
    invalid deltas.
- **Path and location rules:** traversal, absolute or drive paths,
  backslashes, upper case, device names, metadata collisions, depth, length
  and duplicates; staged names with traversal or a hash mismatch; output
  inside the checkout or the staging directory.
- **Publication:**
  - simulated disk-full at 0, 1, 1,000, half and all-but-one byte of a run;
  - interruption before the manifest, validation, promotion and the
    `CURRENT` update;
  - insufficient free space, which is refused before writing;
  - tampered outputs and a malformed `CURRENT`.

  In every case the previous generation stayed current and verified, and no
  staging was left behind. A killed run's staging directory was discarded by
  the next run. Sources were hashed before and after every converter test.

**Owned data.** Each fault run converted `ui` and one campaign map into the
root whose current generation is the full one (`01bf0850…`).

| Case | Bytes written before the fault | Exit | `CURRENT` unchanged | Staging left |
|---|---|---|---|---|
| Disk full at 0 bytes | 2,816 (first chunk refused) | 4 | yes | 0 |
| Disk full at 50 MB | 50,200,192 | 4 | yes | 0 |
| Disk full at 200 MB | 200,043,672 | 4 | yes | 0 |
| Stop before validation | 340,808,632 | 4 | yes | 0 |
| Stop before promotion | 340,808,632 | 4 | yes | 0 |
| Stop before the `CURRENT` update (renamed, never made current) | 340,808,632 | 4 | yes | 0 |

- **Published generation.** After all six cases, every one of its 87,723
  files was hash-identical to the first run.
- **Real kill.** A two-map run was killed by the OS after 12.1 s (Windows
  exit code 1). It left one staging directory with 181 files and
  `CURRENT` unchanged. The next full run (exit 0, 2,082 s) discarded that staging directory,
verified the existing generation, reused it and left no staging behind.
- **Sources.** Each converter run re-hashed its source maps after
  publishing, and all matched.

The driver's planned second movie probe refused to overwrite the existing
report and exited 2. That is the intended no-overwrite behaviour, and the
failure is kept in the private command log.

## 4. Movies and audio plan

### Movies (measured on the host; Wii decoding is open)

`probe_movies.py` ran the user's FFmpeg 6.0 build (gyan.dev "essentials",
`ffmpeg.exe` SHA-256 `e9fd5e71…49db1`). FFmpeg read each file's byte range
inside the image through its subfile protocol, so nothing was extracted.
Every file decoded completely: the decoded frame count equals the Bink
header's, with no decoder messages and exit 0. The tools tree held only an
uncommitted README edit when it ran, and the probe module is unchanged from
`4062b2d6` (SHA-256 `4da4052f…765f` in the JSON).

| Kind | Files | Bytes | Duration | Format | Host decode, real time |
|---|---|---|---|---|---|
| Game movies | 5 | 647,097,636 | 543.5 s (16,290 frames) | Bink 1 rev `i`, 640×480, 29.97 fps; one Bink audio (RDFT) track, 44.1 kHz stereo | 19.8 s in total |
| Demo-launcher videos | 3 | 1,106,183,284 | 1,159.0 s | Bink 1 rev `i`, 720×486 or 640×480; same audio | 62.3 s in total |

The demo-launcher videos belong to the disc's separate demo application,
not to Halo. Whether the port includes them is an **open user decision**.
They are not counted as required parity until that decision is made.

These are candidate encodings of the 16 s game movie, as host transcodes of
the user's own data. The outputs stay private.

| Candidate | Wii decoder candidate | Licence and provenance | Sample bitrate | All five game movies at that rate |
|---|---|---|---|---|
| MPEG-1 video + MP2 audio (`-q:v 4`, 192 kbit/s) | pl_mpeg (single-header MPEG-1/MP2) | MIT. Not yet fetched or reviewed. | 2,279 kbit/s | ≈155 MB |
| Intra-only MJPEG + big-endian PCM16 | libjpeg-turbo, or a small baseline decoder | IJG/BSD-style. Not yet reviewed. | 4,656 kbit/s | ≈316 MB |
| Bink as stored, decoded on the Wii | A port of FFmpeg's clean-room Bink decoders | LGPL-2.1+; static linking into a DOL needs relinkable objects | (source) 9,524 kbit/s mean | 647 MB |

- **Proposal.** Transcode the movies at import time, on the host, with the
  user's own FFmpeg. Use MPEG-1/MP2 or MJPEG, chosen by a Wii decode-speed
  measurement at 640×480 and 29.97 fps. FFmpeg is only an external tool the
  user supplies, so no FFmpeg code enters a Wii binary.
- **Excluded decoders.** RAD's Bink SDK and Nintendo's THP SDK decoder are
  proprietary and are not used.
- **Open:**
  - Wii CPU and memory cost of each decoder;
  - A/V sync;
  - whether to keep each movie's Bink RDFT audio in the transcode, or decode
    it to ADPCM or PCM instead;
  - transcode quality settings (only one 16 s sample was measured, so the
    bitrates are extrapolated);
  - a playback path that respects the 30 Hz simulation.

### Audio

All 72,558 game sound permutations use Xbox ADPCM, which is IMA ADPCM with
36-byte blocks of 64 samples per channel. Its algorithm is public. The
repository's own Linux mixer decodes it (`port/linux/src/dsound_sdl.c`), and
`media_formats.py` matches that decoder.

| Option | Storage | Notes |
|---|---|---|
| **A. Keep Xbox ADPCM at rest, decode on the CPU into PCM16 voices (proposed)** | 1,606,686,372 bytes | Lossless relative to the source and 4:1 in memory. HWS1 already carries it. The runtime decoder must read the block header word as explicit little-endian. |
| B. Pre-decode to big-endian PCM16 | 5,712,662,656 bytes | Simple at runtime, but 3.6× the storage and memory. At most suitable for a small hot set. |
| C. Re-encode to Nintendo DSP-ADPCM for hardware decode | ≈1.6 GB | A second lossy generation. Needs an encoder with reviewed provenance and a DSP voice path; libogc's ASND/AESND voice formats still have to be checked for DSP-ADPCM support. |

- **Open:** the CPU cost of option A for the engine's maximum simultaneous
  voices, measured on hardware, and streaming of the long 44.1 kHz stereo
  permutations from SD.
- **Demo launcher only:** a WMA stream (WMA is proprietary; FFmpeg has an
  LGPL decoder) and two PCM WAVs. Their scope is the same open decision as
  the demo videos.

## Not established

- Any Wii, Dolphin or hardware behaviour. This includes the GX sampling of
  these outputs, FAT rename atomicity on SD, libfat directory performance
  and decode CPU cost.
- Runtime loaders for HWT1, HRA1 and HWS1, and texture or sound residency
  budgets, which belong to HWI-007.
- Conversion of models, collision, model animations, shaders, scripts, BSP
  lightmaps, font glyphs and unicode text, and movies.
- Use of cube maps, volumes and the six oversize textures on GX.
- Canonical Xbox checksums or authenticity of the dump.

HWI-008 stays open (see the JSON `acceptance` block).
