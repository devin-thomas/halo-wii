# Real Halo geometry on emulated Wii (HWI-007A / HWI-016A)

One owned Halo environment section now streams from the emulated SD card and
renders natively through GX in stock Wii-mode Dolphin. It is BSP 12, material
3: 2,990 original triangles over 5,863 vertices. Shading is flat per triangle
for diagnosis; this is not Halo's material, lightmap or gameplay presentation.
[Numeric evidence](2026-10-10-geometry-view.json)

## Build

Clean source `2ba65dea`, build `207ac4aede0d411f`, devkitPPC GCC 16.1.0,
strict C11. `geometry_view.dol` is 543,072 bytes, SHA-256
`5d4e2e51431d086c3bdc55dd1091c465ad7591dc4e67e90e69d19d321fbb4565`. No game
data is embedded. A private manifest on the SD card supplies the input
identities.

## Result: two cold launches, identical

| Check | Each launch |
|---|---|
| Load/use/unload cycles | 4 (3 fixed, 1 interactive), placements alternating by 4 KiB |
| Section CRCs (indices / positions) | `bad05748` / `db11989c`, every cycle |
| Boundary cross-check | All 2,990 triangles agree with the position array |
| Sampled EFB grid at fixed pose | 63/768 on geometry, CRC `aac744a4`, every cycle |
| Released views and handles | 4/4 reject |
| Malformed manifest (duplicate, missing, unknown key) | 3/3 reject |
| Flipped BSP CRC, short tag length | Both reject; MEM2 restored |
| Heap after load vs end | 1,669,488 bytes both (no growth) |
| Interactive input | Orbit, zoom, two resets, exit by START |
| Launch counter read back | 1, then 2 |
| Host exit | 0x0 natural, no forced stop |

A load takes 1.63 s in emulation. Rendering plus `GX_DrawDone` peaks at
305 µs per frame, and CPU FIFO submission peaks at 77,056 bytes per frame. The
Windows event logs show no bugcheck, unexpected shutdown or Dolphin fault during
the runs; that observation covers this session only.

## Memory for this diagnostic

Usable MEM2 is 54,284,128 bytes. The plan charges 50,579,712 bytes, leaving
3,704,416 at the base placement or 3,700,320 at the offset one. The plan
includes:
- the 22 MiB tag slot, holding the tags with the selected BSP at its select
  offset;
- state and sound placeholder reservations;
- two 64 KiB IO buffers;
- the controls and material workspace;
- native position (70,368 B), index (17,952 B) and shade (11,968 B) arrays;
- a 2 MiB reserve.

MEM1 heap use is 1.67 MB, which covers the FIFO, both XFBs and libogc. The state
and sound slots are reservations, not engine pools. This working set is a
diagnostic and shows nothing about whether the complete game fits.

Not measured: IOS use outside the arena, GP FIFO occupancy peak, stack peak
(the sampled depth is 624 bytes) and system fragmentation beyond two placements.

## Development iterations (retained)

The first launch loaded, verified and rendered the section correctly but failed
its own coverage threshold. One eighth of a 16×12 grid was an arbitrary bar,
and this sparse structure covered 8 samples. The second launch used a denser
32×24 grid, a closer camera and a floor of 16 samples, and passed. The
discriminating test is the identical grid CRC after every reload.

## Not established

- Back-face culling is off; the winding of this content is not qualified.
- Halo shaders, textures, lightmaps, collision and gameplay.
- Recognisability was judged from frame dumps only. Renders of game content
  are kept private and recorded here by hash.
- Physical Wii.
