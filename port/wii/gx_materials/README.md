# Asset-free GX materials self-test (HWI-014)

A native libogc/GX diagnostic that renders authored primitives, textures,
alpha, mipmap and TEV fixtures and checks each one in the guest by reading the
EFB back from the CPU. It uses no game data and is not a Halo renderer,
material system or gameplay result. The interactive HWI-014A scene
(`wii_gx_scene`) is a separate target with its own nine checks.

## Build

From the official devkitPro environment:

```sh
python configure.py --wii --wii-devkitpro <devkitPro root> [--wii-probe-frames N]
ninja wii_gx_materials
```

Outputs are `build/wii/gx_materials.{elf,dol,map}` and
`build/wii/gx_materials-build-info.json`.

## What runs

Four repeated load / check / unload cycles run before the interactive loop.
Each cycle allocates every texture, encodes it, flushes it and registers it,
then runs 49 checks and releases everything. The cycles must produce identical
EFB readback CRCs and leave the heap and the MEM2 pool where they started.

Every check renders into its own 64x64 tile with `GX_PeekARGB` or `GX_PeekZ`
probes. A check is one of three kinds:

| Kind | Rule |
|---|---|
| `positive` | Every probe matches the expected colour within the stated tolerance |
| `control_value` | A deliberately different configuration must produce its own, different expected value |
| `control_must_differ` | A deliberately wrong encoding is probed against the correct expectation. At least a quarter of probes must mismatch |

| Area | Positive checks | Controls |
|---|---|---|
| Depth and culling | Near quad wins in either draw order; Z near < far; back/front culling of clockwise/counter-clockwise quads; two planes intersecting in perspective | Depth off; culling none and all; perspective depth off |
| Textures (GX tiled block order, every texel) | I8, IA8, RGB565, RGB5A3 (opaque and 3-bit-alpha texels), RGBA8, CI8 with an RGB565 TLUT, and CMPR converted from authored Xbox/PC DXT1. Alpha channels are checked through `GX_CC_TEXA` | Untiled I8, RGB565 and RGBA8; raw unconverted DXT1; DXT1 converted per block but left in row-major block order |
| Wrap | Repeat, clamp and mirror over texture coordinates 0..2 | The three modes predict different texels |
| Alpha | Src-alpha/inv-src-alpha blending at alpha 128 and 64; alpha test GREATER 128; RGB5A3 translucent texels blended; CMPR three-colour transparent texels cut out by an alpha test | Blending off; alpha test always; cut-out with the test off |
| Mipmaps | 32x32 RGB565 with six distinctly coloured levels: 32, 16, 8, 4 and 2 pixel squares select levels 0 to 4; LOD bias +2; max LOD 1; min LOD 2 | Mipmapping off |
| TEV | Texture x vertex colour (`GX_MODULATE`); two stages adding K0 x a second texture | `GX_REPLACE`; stage 1 configured but `GX_SetNumTevStages(1)` |
| Resource update | Texture rewritten in place, then `DCFlushRange` and `GX_InvalidateTexAll`, samples the new colour | - |

`DXT1 -> CMPR` conversion (`dxt1_to_cmpr`) swaps the colour endpoints to big
endian, reverses the 2-bit index order in each row, and regroups 4x4 blocks
into 2x2 groups per 8x8 tile. Halo's Xbox DXT1 content needs this mapping.
CMPR interpolates in 5/8 + 3/8 steps rather than DXT1's 2/3 + 1/3, so
converted textures shift interpolated colours slightly. The reference decoder
uses the GX weights.

## Resource diagnostics

The log at `sd:/halo-wii-gxm/materials.log` records:

- `OWN` / `RELEASE`: every buffer GX or VI reads. These are the FIFO, both
  XFBs, every texture and the TLUT, with address, size, region and cache
  policy. Registration rejects null, zero-length, wrapping, misaligned (base
  or length not a 32-byte multiple), outside-MEM1/MEM2 and overlapping ranges.
  It also rejects a full registry. Texture objects are created only after the
  data is confirmed to lie inside one owned range. GX is idled with
  `GX_DrawDone` before anything is released.
- Textures live in a 64 KiB pool carved from the MEM2 arena. Each allocation
  is followed by a guard line, checked on release. The TLUT and per-load
  descriptors come from the heap. CPU writes are followed by `DCFlushRange`,
  then `GX_LoadTlut` / `GX_InvalidateTexAll`.
- `NEGATIVE`: 17 deliberate invalid cases, each of which must be detected for
  its stated reason. They cover misaligned base and length, overlap with a
  texture and with the FIFO, a range outside MEM1/MEM2, a wrapping range,
  zero length and null. They also cover a texture read from unowned memory, a
  read past the owned end, a double release, an unknown handle, a leaked
  range, a one-byte overrun into a guard, pool exhaustion, heap allocation
  failure and a full registry.
- `TEXBYTES`: tiled size per texture against libogc's
  `GX_GetTexBufferSize`. A disagreement is reported, and the allocation takes
  the larger value.
- `CYCLE`: checks, CRC, heap before/loaded/after, texture bytes, pool use, CPU
  upload time, check time and FIFO bytes per check pass.
- `FIFO`, `TIMING`, `EFB`, `XFB`, `PEEK`, `STACK`, `HEAP`: per-frame gallery
  FIFO submission (PI write-pointer delta), submit and EFB-to-XFB copy time,
  framebuffer sizes, and heap growth across the loop and the whole run.

`sd:/halo-wii-gxm/runs.txt` counts launches. An invalid counter is preserved,
not reset.

## Controls

After the cycles, every positive configuration is drawn each frame as a
gallery. The HUD shows the build ID and `checks_passed-total-frame`.

| Input | Effect |
|---|---|
| A | Toggle a trilinear mipmapped plane receding in perspective |
| B | Toggle the resource overlay: MEM2 pool use, FIFO peak and registry occupancy as bars |
| START / Remote HOME | Exit |

## Limits

The checks run in an emulator. Dolphin does not model the CPU data cache by
default, so a missing `DCFlushRange` or `GX_InvalidateTexAll` would not fail
here; real-Wii cache and DMA behaviour stay untested until run on a console.
Dolphin chooses mip levels with the host GPU's LOD calculation. The checks use
power-of-two minification at integral LODs and nearest-mip filtering, where
that is exact. Fractional LOD, trilinear blend weights and edge/anisotropic LOD
are not verified. Peek and copy times are emulated time, not hardware timing.
