# Split-alpha textures: DXT3/DXT5 as CMPR plus an exact alpha plane (HWI-008F)

A new texture profile, `gx-splitalpha-v1`, stores DXT3 and DXT5 bitmaps as
CMPR colour plus a separate alpha plane. Before this, `gx-baseline-v1`
expanded them to RGBA8 at four times their source size. The baseline
profile stays available, and every other format converts to the same bytes
under both profiles.

On Prisoner, all 10 environment base maps are now resident. Before, the
render path was 5,802,612 bytes short and drew 8 of them lightmap-only.
Game state still equals the i686 host in all four evidence launches.
[Numeric evidence](2026-10-10-split-alpha-textures.json)

**Result:**

- **Texture bytes for the owned set:** 1,587,808,352 before, 967,769,088
  after (−620,039,264). DXT3 went from 722,167,616 to 180,848,320 bytes,
  DXT5 from 125,974,784 to 47,254,816.
- **Fidelity, checked on every texel of all 2,973 DXT3/DXT5 textures**
  (211,953,332 texels):
  - alpha is exact for every texel;
  - colour is within the DXT1 tolerance accepted in HWI-008B: at most 11
    per channel, with 69.1% of texels exact.
- **Prisoner residency:** 10 of 10 base maps resident, 2,316,960 bytes
  (RGBA8 needed 9,088,384). The render path owns 2,865,752 bytes at peak.
- **Game state:** simulation `127656ef` and whole state `6d88af44` in every
  launch. The host comparison passes 18/18 runs in all four launches.
- **Pixels:** at all 5 poses, every covered sample now changes when the base
  maps are dropped. Before, only 41–69% did.
- **The alpha path through TEV is exact:** with the same two base maps
  resident, the base-alpha view gives the same CRC under RGBA8 and
  split-alpha at all 5 poses.

## Build and content

| Item | Value |
|---|---|
| Source | Clean `a4a4b0a9` (profile `36745438`, loader `8a7df1cf`, renderer `a4a4b0a9`) |
| Build ID | `67c8dc836ab55999` |
| Toolchain | devkitPPC GCC 16.1.0, binutils 2.46.0 |
| `engine.dol` | 4,100,672 bytes, SHA-256 `50132f8a426ca91a92b086c8eb7d7876149f6b7c0fe75e6af01ef6dfada50438` |
| `content_loaders.dol` | SHA-256 `2e385b2497c84f2f4bc87c835289b0d6ed101cd2270af5b71bac7e286e6be83a` |
| Host reference | Debian clang 19.1.7, i686, built from the same commit |
| Generation | `6e7b87372c08e3fa`, 91,397 outputs |
| Staged for Prisoner | 13 files from that generation (`render_stage.py`, private) |

Tests pass on Python 3.14 and 3.11.

## The profile

### Colour

DXT3 and DXT5 colour blocks always decode in the four-colour mode. CMPR, like
DXT1, switches to three colours plus transparency when c0 <= c1. Each block
is therefore rewritten:

| Block | Rewrite | Owned set |
|---|---|---:|
| c0 > c1 | kept | 9,149,973 |
| c0 < c1 | endpoints swapped, indices 0↔1 and 2↔3 | 2,972,902 |
| c0 == c1 | every index set to 0 (every palette entry is c0) | 1,126,377 |

**Why the colour stays within tolerance:**

- **Swapping keeps the palette.** The DXT palette satisfies
  `DXT(c0, c1)[i] == DXT(c1, c0)[i ^ 1]`. After the swap the block has
  c0 > c1, so CMPR decodes it in the four-colour mode.
- **The only remaining difference is the middle colours.** GX blends them at
  5/8 + 3/8, DXT at 2/3 + 1/3.
- **The bound was checked exhaustively** for every pair of expanded 5-bit
  and 6-bit endpoint values (`gx_texture.interpolation_error_table`): the
  error is at most |c0−c1|/24 + 1 and at most 11 per channel, with no pair
  over the bound. This is the tolerance HWI-008B accepted for DXT1.
- **Exact cases:** endpoint texels and c0 == c1 blocks.

**What the old behaviour would have done:** without the rewrite, the 1.1
million uniform blocks would have turned every index-3 texel transparent
black.

### Alpha

- **DXT3:** the explicit 4-bit alpha goes into I4. GX expands n to 17n, as
  the reference decode does.
- **DXT5:** stored in I8 as the reference decode's value (`_dxt5_alphas`):
  - the 8-value mode `((7−i)·a0 + i·a1) // 7` when a0 > a1;
  - otherwise the 6-value mode `((5−i)·a0 + i·a1) // 5`, plus 0 and 255.

  Division rounds down. This is the same rule HWI-008B checked RGBA8
  against. Decoders that round to nearest differ by at most 1.
- **DXT5 in I4:** allowed only when every value of the texture is a
  multiple of 17, because then it is exact and smaller.
- **No plane:** a texture whose alpha is 255 at every texel of every image
  has no plane, and its header records that.
- **On the owned set:** all 2,816 DXT3 textures have an I4 plane and all 157
  DXT5 textures an I8 plane. No texture had constant 255 alpha or only
  multiples of 17. Authored fixtures cover both of those paths.

### Container

HWT1 version 2 adds an explicit, versioned alpha section in header bytes 32
to 47:

- section version;
- kind (no plane, or plane present);
- the plane's GX format;
- its offset and size.

The plane holds one image per colour image, in the same order. The Wii
loader also checks that every CMPR block is in the four-colour form. Version
1 files still load: every non-DXT3/DXT5 output (88,424 files) is
byte-identical to `gx-baseline-v1`. See the
[guide](../CONTENT-PIPELINE.md#formats-all-big-endian).

## Conversion

| Run | Host | Converter seconds | Wall seconds | Generation |
|---|---|---:|---:|---|
| R1 | Research (M5 Max, macOS, Python 3.12) | 145.0 | 145.1 | `6e7b87372c08e3fa` |
| R2, fresh root | Research | 141.7 | 141.8 | `6e7b87372c08e3fa` |
| T1 | Titan (Windows 11, Python 3.14; shared) | 1,438.0 | 1,438.9 | `6e7b87372c08e3fa` |

- **Byte identity:** all three output trees have the same digest:
  91,399 files, 3,070,519,883 bytes, tree SHA-256 `b2173567…`.
- **Compared with the baseline:** `gx-baseline-v1` took 272.3 s and 272.9 s
  on Research (HWI-008C). The split profile is faster because DXT3/DXT5 are
  no longer decoded to RGBA8.
- **Titan was shared** with other agents during T1, and with this ticket's
  first Dolphin batch. That batch was voided by a session crash and run
  again.
- **Staging the maps** from the image on Research took 2.2 s.

**Whole-set verification** (`verify_textures.py`, 16 workers on Research,
57.5 s): every texel of every image of every DXT3/DXT5 output was read back
from the published bytes by the independent GX decoder and compared with the
reference decode of its source.

| Kind | Textures | Texels | Exact texels | Alpha exact | Max error R, G, B, A |
|---|---:|---:|---:|---:|---|
| DXT3, I4 plane | 2,816 | 180,463,732 | 127,309,031 | 180,463,732 | 11, 11, 11, 0 |
| DXT5, I8 plane | 157 | 31,489,600 | 19,227,719 | 31,489,600 | 11, 11, 11, 0 |

### Bytes

| | `gx-baseline-v1` | `gx-splitalpha-v1` |
|---|---:|---:|
| All textures | 1,587,808,352 | 967,769,088 |
| DXT3 (2,816) | 722,167,616 | 180,848,320 |
| DXT5 (157) | 125,974,784 | 47,254,816 |
| Prisoner, all textures | 51,278,368 | 30,288,864 |
| Prisoner, environment base maps (10) | 9,088,384 | 2,316,960 |
| a10, all textures | 111,677,824 | 72,005,952 |
| a10, environment base maps (122, all BSPs) | 103,822,464 | 27,807,296 |

For a10, only the bytes were counted: its BSPs are not rendered yet.

## Prisoner in the engine

### Residency and memory

Map load (every load, both hosts): `base_maps=10 base_resident=10
base_bytes=2316960 base_not_resident_bytes=0 base_alpha_planes=10`.

| Item | HWI-016B | Now |
|---|---:|---:|
| Base maps resident | 2 of 10 (1,747,840 B, RGBA8) | 10 of 10 (2,316,960 B, CMPR + I4/I8) |
| Shortage for full residency | 5,802,612 | none |
| Provisional base budget | 2 MiB | 3 MiB (3,145,728) |
| Render path owned at peak | 2,287,732 | 2,865,752 |
| Engine image | 21,349,792 | 21,353,248 |
| MEM1 free after GX | 1,921,024 | 1,916,928 |
| MEM1 free after engine start-up | 839,680 | 835,584 |
| Heap growth in MEM1 | 454,656 | 491,520 |
| Heap growth in MEM2 | 2,002,944 | 2,482,176 |
| MEM1 left once the heap reaches MEM2 | 348,160 | 344,064 |
| MEM2 spare above the 2 MiB reserve, after | 983,040 | 503,808 |

Everything is released at each unload: the heap is flat across cycles, and
`owned_now=0` at the end.

**Where the shaders use base-map alpha:**

- On the Xbox, the environment diffuse pass reads the base map's alpha only
  for shader types blended and blended base specular. There it blends the
  detail maps and is the specular mask.
- **On Prisoner, all 11 environment materials are type normal.** No
  material reads base-map alpha, so the full frame binds no plane.
- The renderer adds the plane stage wherever a material's type does use it.
- Each pose also draws a base-alpha view, which exercises the plane through
  TEV on every base map.

### Fixed poses (Titan, EFB, 32×24 = 768 samples)

| Pose | Covered | Changed by the base map, HWI-016B → now | Base alpha < 255 | Cull back (front/back) | Cull front (front/back) | State unchanged |
|---|---:|---|---:|---|---|---|
| Engine, tick 30 | 768 | 392 → **768** | 766 | 768 / 0 | 0 / 547 | yes |
| Engine, tick 150 | 768 | 354 → **768** | 768 | 768 / 0 | 0 / 71 | yes |
| Engine, tick 300 | 768 | 313 → **768** | 768 | 768 / 0 | 0 / 98 | yes |
| Override, high | 570 | 306 → **570** | 569 | 570 / 0 | 0 / 564 | yes |
| Override, mid | 768 | 533 → **768** | 767 | 768 / 0 | 0 / 501 | yes |

"Changed by the base map" counts samples that differ between the full draw
and a lightmap-only draw. Every covered sample is now textured.

**Determinism:**

- **Within a host:** pose CRCs (full, lightmap-only, base-only, base-alpha)
  are equal in all 4 launches on each host (2 full and 2 poses-only).
- **Across hosts:** they differ, as in HWI-016B, because D3D and Vulkan
  filter textures differently. The lightmap-only CRCs on Research equal
  HWI-016B's Research values.

| Pose | Titan full | Research full |
|---|---|---|
| Engine, tick 30 | `62ff41b0` | `1de20107` |
| Engine, tick 150 | `f815b5a5` | `4e99f685` |
| Engine, tick 300 | `2fd595e3` | `e3eb3c53` |
| Override, high | `76e27f63` | `a6357304` |
| Override, mid | `acf4671e` | `972ab0b9` |

**Frame dumps** (poses-only launches):

- The 4 final pose frames are identical across the two launches on Titan.
- The early frames shift by one, because the frames are presented without
  waiting for the retrace (as noted in HWI-016B).
- 202 of 209 distinct frame images appear in both launches.

### Profile comparison at equal residency (Titan, `base_limit=2`)

Both launches keep the same two base maps (one DXT5, one DXT3), first as
RGBA8 (HWI-016B's staged files) and then as split-alpha.

| Pose | Base-alpha CRC, RGBA8 / split | Lightmap-only | Full and base-only |
|---|---|---|---|
| Engine, tick 30 | `5e793a3a` / `5e793a3a` | equal | differ (CMPR colour) |
| Engine, tick 150 | `5bc991fd` / `5bc991fd` | equal | differ |
| Engine, tick 300 | `5f7e9efe` / `5f7e9efe` | equal | differ |
| Override, high | `f38736c0` / `f38736c0` | equal | differ |
| Override, mid | `055bd740` / `055bd740` | equal | differ |

**What this shows:**

- **The alpha is exact through the sampler and TEV.** The plane reads back
  the same as RGBA8's own alpha.
- **Only the colour differs**, by CMPR's interpolation.
- **The new build changes nothing for RGBA8 content.** The RGBA8 launch
  reproduces HWI-016B's Titan full, lightmap-only and base-only CRCs at all 5
  poses.

## Game state against the i686 host

The Wii draws every frame. The host draws nothing and replays each launch's
measured retrace frame times at that launch's arena placement (launch 2 of
each host is at +4 KiB).

| Launch | Runs, simulation equal | Runs, whole state equal | Allocation checkpoints | Ticks |
|---|---:|---:|---:|---:|
| Titan 1 | 18/18 | 18/18 | 1,404/1,404 | 300/300 |
| Titan 2 | 18/18 | 18/18 | 1,404/1,404 | 300/300 |
| Research 1 | 18/18 | 18/18 | 1,404/1,404 | 300/300 |
| Research 2 | 18/18 | 18/18 | 1,404/1,404 | 300/300 |

Every launch ends at simulation `127656ef`/`73904c7f` and whole state
`6d88af44`/`fb5dcbf2`, with the heap flat. The pose phase also ends at
`127656ef`.

**A wrong offset fails the comparison:** a first host run for Titan 2 at
offset 0 failed only in the tag-slot digest (`tags_end`). It was voided and
run again at the launch's +4 KiB offset.

## Two cold launches per host

| Check | Titan 1 | Titan 2 | Research 1 | Research 2 |
|---|---|---|---|---|
| Guest result (`END`) | pass, 0 failures | pass | pass | pass |
| Base maps resident | 10/10 | 10/10 | 10/10 | 10/10 |
| Simulation / whole state | `127656ef` / `6d88af44` | same | same | same |
| Persistence | none, counter 1 | previous digest equal, counter 2 | none, 1 | equal, 2 |
| Host exit | `0x0`, 123.5 s | `0x0`, 122.4 s | `0x0`, 120.6 s | `0x0`, 119.7 s |
| Dolphin processes left | 0 | 0 | 0 | 0 |
| OS stability | no instability events | same | no instability events (macOS checks) | same |

- **Backends:** Titan used D3D and Research used Vulkan, both stock Dolphin
  2609 with a fresh profile per launch.
- **Poses-only launches:** two per host, all exit `0x0`. Titan took 27.4 s
  and 32.3 s, Research 21.6 s twice.
- **Physical Wii:** untested.
- **A voided batch:** the first Titan evidence batch was cut off by a crash
  of the agent session, not of Dolphin or the guest. Its orphaned Dolphin
  was stopped and the batch was voided. Nothing from it is used here.

## Loader self-test (Titan, two cold launches)

Cases came from the split-alpha generation (`content_loader_cases.py`):

- 227 cases: 79 valid and 148 malformed. The malformed cases include 16
  alpha-section and colour-block defects.
- Each cycle passes 227/227, including 74/74 textures.
- 751 GX images and 131 mip levels were read back from the EFB with 0
  mismatches, maximum error 0. Split textures are drawn as TEV combines
  them: CMPR colour in stage 0, the plane's alpha in stage 1, and the alpha
  checked through `APREV`.
- Wrong-format controls: 38/42 detected.
- Cycle CRC `a22a1a5f` in both cycles of both launches; no heap growth.
- Host exits `0x0` (23.5 s, 23.2 s), persistence passed, no OS events.
- The host build of the same loader passes 227/227 on Research.

## Timings

| Step | Titan | Research |
|---|---:|---:|
| Full conversion | 1,438.9 s (shared) | 145.1 s, 141.8 s |
| Verify every DXT3/DXT5 texel | — | 57.5 s (16 workers) |
| Wii build after reconfigure | 90 s | not built there |
| Host reference build | 202.8 s | — |
| Host reference, per launch | 10.9–31.3 s | — |
| Dolphin, two full launches (batch) | 289 s | 252 s |
| Dolphin, two poses-only launches (batch) | 89 s | 55 s |
| Dolphin, profile comparison (one launch each) | 67 s, 39 s | — |
| Dolphin, loader self-test ×2 (batch) | 100 s | — |

## Visual review (private)

Four frame dumps from the poses-only launches are in the private run folder.
They are never committed or published.

- **Engine camera, tick 300 (Titan):** the corridor from HWI-016B's review
  frame. The walls, panels, ceiling and hex floor are now textured where
  HWI-016B drew them lightmap-only.
- **Diagnostic override, high (Titan, and Research):** the whole BSP from
  above a corner, textured throughout.
- **Diagnostic override, mid (Titan):** textured pillars, walkways and
  walls. The magenta placeholder panels are unchanged.

## Limits and remaining work

- **Base-map alpha has no consumer yet.** Detail maps, the specular mask and
  alpha testing are HWI-032's. On Prisoner no environment shader reads
  base-map alpha, so the full frame does not use the plane yet. The
  base-alpha view and the equal-residency comparison prove the plane path.
- **Other maps:** campaign maps are counted (a10) but not rendered.
  Residency on larger maps, and streaming, belong to HWI-032's texture
  cache. a10's 122 base maps alone are 27.8 MB in this profile.
- **Budget:** the 3 MiB base budget is provisional. MEM2 spare after
  Prisoner's load is 503,808 bytes.
- **Emulator only:** physical Wii and hardware GP cost are untested
  (HWI-040).
