# Real-geometry diagnostic (HWI-007A / HWI-016A)

Streams one owned Halo BSP section from the emulated SD card and draws its
original triangles through GX with flat per-triangle shading. It renders no
Halo materials, lightmaps, collision or gameplay, and the repository contains no
game data.

## Inputs

`sd:/halo-wii-geometry/section.txt` holds one `key value` pair per line. Every
key is required exactly once, and unknown or duplicate keys reject:
`tag_file`, `bsp_file`, `tag_bytes`, `tag_crc32`, `bsp_bytes`, `bsp_crc32`,
`map_bytes`, `bsp_ordinal`, `material`, `surfaces`, `vertices`, `index_crc32`
and `position_crc32`. `tools/wii/select_geometry_section.py` produces the
identity values from privately derived tag and BSP blobs.

## What one run does

1. Plans MEM2 with the existing engine reservations (tag slot, state and
   sound placeholders, IO buffers, controls, material workspace) plus native
   position, index and shade arrays.
2. For each of three load/use/unload cycles plus an interactive one:
   - streams tags and BSP with CRC checks and binds the BSP and material
     views;
   - copies positions with integer stores and cross-checks every triangle
     through `cache_material_get_surface_positions`;
   - checks the section CRCs and renders;
   - samples a 32×24 EFB grid at a fixed pose, which must match the first
     cycle exactly;
   - releases the views, which must then reject.
3. Loads with a flipped BSP CRC and with a short tag length. Both must reject
   and restore MEM2.

Placements alternate by 4 KiB.

`sd:/halo-wii-geometry/view.log` records the plan, the MEM2 charge and its
remainder, load time, the CRCs, the grid result, input, the faults, the heap
(which must not grow), FIFO submission and sampled stack depth.

## Controls (interactive cycle)

Stick: orbit. C-stick: zoom. B: reset. START or HOME: exit. START is honoured
after 240 frames, once the camera has been driven.

## Limits

- Back-face culling is off, because winding is not yet qualified for this
  content.
- The state and sound slots are placeholder reservations, not engine pools.
- Physical Wii is untested.
