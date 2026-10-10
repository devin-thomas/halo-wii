# Asset-free GX scene (HWI-014A)

A native libogc/GX diagnostic: one indexed colour cube, an intersecting
satellite cube, a checkered floor and a 3x5 bitmap HUD. It uses no game data
and is not a Halo renderer, engine integration or gameplay result.

## Build

From the official devkitPro environment:

```sh
python configure.py --wii --wii-devkitpro <devkitPro root> [--wii-probe-frames N]
ninja wii_gx_scene
```

Outputs are `build/wii/gx_scene.{elf,dol,map}` and
`build/wii/gx_scene-build-info.json`. The probe target `wii_probe` and its
artifact paths are unchanged. `--wii-probe-frames` sets an auto-exit backstop
shared by both diagnostics.

## Controls

| Input | Effect |
|---|---|
| GC stick / Remote D-pad | Yaw and pitch the main cube |
| GC C-stick vertical / Remote +/- | Camera distance (3 to 14) |
| A | Toggle automatic spin |
| B | Reset pose |
| START / Remote HOME | Exit |

HUD rows: build ID (yellow); `frame-calibration_passed` (white);
`held_buttons-pads_present-yaw_degrees` (cyan).

## Self-checks and accounting

Before the interactive loop the scene renders eight fixed poses into the EFB
and reads pixels back with `GX_PeekARGB`/`GX_PeekZ`: front face, backdrop,
clear colour, pitch (top face visible), yaw (left face visible), and a
cube/backdrop depth order. Two controls must change the result: with culling
off a back-facing quad becomes visible, and with depth testing off the backdrop
overdraws the cube. All nine checks must pass.

The log at `sd:/halo-wii-gx/scene.log` records:

- `OWN` lines for every buffer GX or VI reads (FIFO, two XFBs, vertex
  arrays) with address, size, alignment, region and cache policy. Null,
  misaligned, out-of-MEM1/MEM2 and overlapping ranges are rejected as
  `INVALID_RANGE`, as are GX arrays outside an owned range.
- `FIFO`: per-frame CPU submission from the PI write pointer. It excludes
  the EFB copy and is not a GP occupancy peak.
- `TIMING`: CPU submit plus `GX_DrawDone`, and frame interval.
- `STACK`: sampled frame addresses, a lower bound rather than a peak.
- `HEAP`: `mallinfo` in-use bytes after initialisation and at exit. The run
  fails if the loop grows the heap.

`sd:/halo-wii-gx/runs.txt` counts launches. It is read back after each write;
an invalid file is preserved rather than overwritten.

The checks run in an emulator. Real-Wii cache/DMA behaviour, VI output on a
display, and GP throughput on hardware remain untested until run on a console.
