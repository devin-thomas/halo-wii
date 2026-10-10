# GX materials, textures and resource diagnostics (HWI-014)

A native GX self-test renders authored primitives, textures, alpha, mipmap and
TEV fixtures in stock Wii-mode Dolphin. The guest decides each check itself by
reading the EFB back from the CPU. Every area has controls that must change the
answer, and a resource registry must catch 17 deliberate invalid cases. No game
data is used, and this is not a Halo renderer or a hardware result.
[Numeric evidence](2026-10-10-gx-materials.json)

## Build

Clean source `55bb5cba59ecbd1b25a9cd039db68cee83351cf3` (`source_dirty:
false`), build ID `5d43e98b79f17ffe`, devkitPPC GCC 16.1.0, GNU ld 2.46, strict
C11 `-Wall -Wextra -Werror`. The commands were
`python configure.py --wii --wii-devkitpro <root> --wii-probe-frames 1800`, then
`ninja wii_probe wii_gx_scene wii_gx_materials wii_geometry_view` from an empty
`build/wii`. The new target is `wii_gx_materials`. It is separate from the
HWI-014A scene so that scene keeps its nine-check contract.

| Artifact | Bytes | SHA-256 |
|---|---|---|
| gx_materials.dol | 512,224 | `e6924da49022f37130ff403ac7bc32552ebac9d26b65ed6a44c21ad6ddf08841` |
| gx_materials.elf | 2,484,044 | `96cf258f75c6c56ade835655a437948c7a209965191d788a17f1560d88ef498a` |
| gx_materials.map | 690,566 | `f22788a495ee9aef1c6e43f668188a324b863eaa80abad62a54c0bbbba54f208` |

The build-only allowlist now covers 17 files. Staging and
`inspect --require-clean` on these outputs: PASS, 0 problems, 0 findings. The
unit tests under `tools/wii` pass 268/268 with Windows Python 3.14. Under MSYS2
Python they pass 266/268; the two failures are pre-existing path-separator
cases in unrelated inspectors.

## Runs: two cold launches, stock Dolphin, Wii mode

`tools/wii/run_dolphin.py --scenario gx_materials --efb-access --runs 2`
created a new isolated profile with stock clock and memory, no cheats, D3D at
1x, CPU EFB access enabled and an authored DTM (virtual GC pad: A, B, START).
Dolphin.exe SHA-256 was `1ed5e3f6…0dac`. An exclusive host-wide Dolphin lock
was held for the batch.

| Outcome | Run 1 | Run 2 |
|---|---|---|
| Guest scope | pass | pass |
| EFB checks (49 per cycle × 4 load cycles) | 196/196 | 196/196 |
| Cycle readback CRC | `53843a35` ×4 | `53843a35` ×4 |
| Deliberate invalid cases detected | 17/17 | 17/17 |
| Unexpected resource failures, guard failures, leaks | 0, 0, 0 | 0, 0, 0 |
| Heap growth (cycles, display loop, whole run) | 0 | 0 |
| Input (A spin at frame 445, B overlay at 541, START exit) | observed | observed |
| Launch counter read back / prior log preserved | 1 / n/a | 2 / yes |
| Host exit | 0x0, natural, 14.7 s | 0x0, natural, 15.2 s |
| Forced stop | no | no |

**Determinism.** Both runs produced 667 frames, and their frame dumps are
byte-identical (first and last hashes match). The cycle CRC also matched in all
four development launches.

**Host and OS.** The Windows event logs show no bugcheck, Kernel-Power,
unexpected-shutdown or Dolphin fault/hang events in the batch window
(12:55:37–12:56:18 UTC); the last boot was 2026-10-08. This covers this session
only. The exit result is scoped to this DOL and configuration. It does not touch
the deferred original-probe teardown fault (HWI-050).

## Rendering checks

Each check draws into its own 64×64 tile, waits on `GX_DrawDone` and probes
with `GX_PeekARGB`/`GX_PeekZ`. Texture checks probe the centre of every texel.
Tolerance is ±6 per channel, or ±12 for interpolated CMPR texels. Each cycle
has 34 positive checks, 15 controls and 2,678 colour probes.

| Area | Positive result | Control result |
|---|---|---|
| Depth | Near quad wins in both draw orders; Z near `0x400000` < far `0xBFFFFF` < clear | Depth off: the later far quad wins |
| Culling | Back cull keeps the clockwise quad only; front cull keeps the counter-clockwise quad only | Cull none shows both; cull all shows neither |
| Perspective depth | Two planes crossing at the tile centre: each wins on its own side | Depth off: the last plane covers both sides |
| I8, IA8, RGB565, RGB5A3, RGBA8 | Every texel exact (max error 0); alpha channels via `GX_CC_TEXA` exact | Untiled I8, RGB565 and RGBA8 matched only 32/128, 16/64 and 0/64 texels |
| CI8 + RGB565 TLUT | 64/64 exact | - |
| CMPR from DXT1 | 240/240 colours and 256/256 alphas exact, including interpolated texels at GX's 5/8 + 3/8 weights and three-colour transparent texels | Raw DXT1 bytes matched 0/240; converted blocks in DXT1 row-major order matched 120/240 |
| Wrap | Repeat, clamp and mirror over s,t ∈ [0,2]: 16/16 each | The three modes predict different texels |
| Blend | Src-alpha / inv-src-alpha at α=128 and α=64 exact; RGB5A3 translucent texels over a background 64/64 | Blending off gives the source colour |
| Alpha test | GREATER 128 rejects α=64 and keeps α=200; CMPR cut-out shows the background through all 16 transparent texels | Test always draws both; cut-out off: 0/16 transparent texels show the background |
| Mipmaps | 32/16/8/4/2-pixel squares select levels 0–4; LOD bias +2, max LOD 1 and min LOD 2 select levels 2, 1 and 2 | Mipmapping off selects level 0 |
| TEV | Texture × vertex colour 64/64 (max error 1); two stages adding K0 × a second texture 64/64 (max error 2) | `GX_REPLACE` gives the bare texture; stage 1 configured but `NumTevStages(1)` gives stage 0 only |
| Texture update | An in-place CPU rewrite, then `DCFlushRange` and `GX_InvalidateTexAll`, samples the new colour | - |

**DXT1 to CMPR conversion.** The CMPR checks double as the conversion test
for Halo's Xbox DXT1 content. The converter swaps the endpoints to big endian,
reverses each row's 2-bit index order, and regroups 4×4 blocks into 2×2 groups
per 8×8 tile. Converted textures will shift interpolated colours slightly,
because GX uses 5/8 + 3/8 weights where DXT1 uses 2/3 + 1/3.

**Mipmap limits in Dolphin.** Dolphin selects levels with the host GPU's LOD
calculation. The checks use power-of-two minification at integral LODs with
nearest-mip filtering, where that agrees with hardware. These are not verified:
- fractional LOD and trilinear blend weights (the A-toggled trilinear floor is
  visual only);
- edge LOD and anisotropic filtering;
- LOD for non-axis-aligned quads.

Dolphin's arbitrary-mipmap detection is relevant to deliberately distinct
levels like these. It changes nothing at 1x internal resolution.

## Resource diagnostics (run 2)

Every buffer GX or VI reads is registered with its address, size, region and
cache policy. A texture object is created only after its data is confirmed to
lie inside an owned range. GX is idled before any release.

| Resource | Bytes | Where / cache policy |
|---|---|---|
| GX FIFO | 262,144 | MEM1; zeroed, then `DCFlushRange` |
| XFB 0 / XFB 1 | 614,400 each | MEM1, K1 uncached, written only by the GX copy |
| EFB | 640×480 RGB8_Z24 | Embedded in the GPU, not main memory |
| Textures (15) | 4,704 in GX layout, 4,768 allocated | 64 KiB MEM2 pool with a 32-byte guard after each; CPU write, then `DCFlushRange` and `GX_InvalidateTexAll` |
| TLUT | 32 | MEM1 heap; `DCFlushRange`, then `GX_LoadTlut` |
| Per-load descriptors | 456 heap bytes | Freed on unload |

Measured in this scenario:

- **Pool.** Peak 6,912 bytes in cycle 1, including guards and negative-case
  scratch, and 5,248 in later cycles. The MEM2 arena low pointer was restored
  at exit.
- **Heap.** 1,669,488 bytes before the cycles and 1,669,944 at peak. Usage
  returned to exactly 1,669,488 after each of the four cycles and after the
  display load; there was no growth over the 667-frame loop.
- **FIFO, CPU submission** (PI write-pointer delta, excluding the EFB copy):
  - at most 864 bytes per check pass;
  - about 24.7 KB per check cycle;
  - 45,914 bytes per frame on average for the full gallery, peaking at 47,200.
- **Time, emulated.** These figures come from the emulated timebase, not
  hardware timing:
  - CPU upload (encode and flush) took 454 µs per cycle, and the 49 checks took
    2.19 ms;
  - gallery submit plus `GX_DrawDone` took 723 µs on average (726 µs max);
  - the EFB-to-XFB copy took 2 µs and an EFB peek about 150 ns. Both are
    Dolphin artefacts, not costs.
- **Stack.** 80 bytes deepest sampled frame. This is a sample, not a peak.

**Deliberate invalid cases, all detected for the stated reason.** Rejections
with a reason: misaligned base, length not a multiple of 32 bytes, overlap with
a texture, overlap with the FIFO, outside MEM1/MEM2, wrapping range, zero
length, null, texture data in unowned memory, read past an owned end, double
release, unknown handle and a full registry. Detected by other means: a leaked
range counted by the leak check, a one-byte overrun caught by the guard, MEM2
pool exhaustion and a heap allocation failure.

**libogc finding.** `GX_GetTexBufferSize(8, 8, GX_TF_CI8, …)` returns 128.
The GX layout (8×4 texels per 32-byte tile) gives 64, and the CI8 texture
samples exactly from those 64 bytes. The other 14 sizes, including the
six-level mip chain (2,784 bytes), agree. Allocations take the larger value so
neither interpretation can read outside the owned range. The disagreement is
reported on every run.

## Development launches (dirty source, retained)

All four reported 196/196 checks, 17/17 negatives, CRC `53843a35`, host 0x0
and no forced stop.

1. **dev-1** failed its guest scope only on the libogc size cross-check above,
   which the first build treated as an error. The fix allocates the larger
   size and reports the disagreement.
2. **dev-2** added a real "stage 1 configured but disabled" TEV control. The
   earlier one re-ran the modulate case. dev-2 passed.
3. **dev-3** changed the pose of the A-toggled trilinear debug plane. Frames
   showed it edge-on or off-screen.
4. **dev-4** stopped tuning rotations and used the viewport and identity-view
   convention of the perspective check. The floor is visible, with mip-level
   colour bands. This is visual only.

After the final run, the runner's log-line parser was changed so a repeated
key (the guest's `TIMING ... max=… max=…`) keeps both values. That parser did
not affect any pass/fail decision. The timing figures above come from the raw
guest log.

## Not established

- Physical Wii. Dolphin does not model the CPU data cache by default, so a
  missing `DCFlushRange` or `GX_InvalidateTexAll` cannot fail here. Real-Wii
  cache/DMA coherence, VI output, GP throughput and controller latency stay
  open until run on a console.
- Fractional and trilinear LOD, edge LOD and anisotropy.
- GP FIFO occupancy peak and stack peak.
- DXT3/DXT5, which have no GX equivalent and need a conversion decision.
- Lighting, fog, indirect textures and EFB-to-texture copies.
- Halo shaders, materials, lightmaps and any game content.
