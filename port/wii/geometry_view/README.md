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
   - decodes each triangle's stored vertex normals and records whether they lie
     along or against the right-hand corner-order normal;
   - checks the section CRCs and renders with back-face culling;
   - samples a 32×24 EFB grid at fixed poses under no culling, `GX_CULL_BACK`
     and `GX_CULL_FRONT` (see Winding below). The samples must match the
     first cycle exactly;
   - releases the views, which must then reject.
3. Loads with a flipped BSP CRC and with a short tag length. Both must reject
   and restore MEM2.

Placements alternate by 4 KiB.

`sd:/halo-wii-geometry/view.log` records the plan, the MEM2 charge and its
remainder, load time, the CRCs, the grid result, the per-mode cull comparison
and draw times, the normal orientation counts, input, the faults, the heap
(which must not grow), FIFO submission and sampled stack depth.

## Winding

Halo treats a triangle as front-facing when its corners run clockwise as seen
by the viewer (`render_camera_triangle_frontfacing` in
`source/render/render_cameras.c`; the Xbox environment passes cull
`D3DCULL_CCW`). On every triangle of this section, the stored vertex normals
oppose the right-hand corner-order normal, which is that convention. GX also
treats clockwise-to-viewer as front-facing, so the original corner order is
drawn with `GX_CULL_BACK` and no indices are reversed.

Each cycle re-qualifies this choice in the EFB. A classified pass colours each
triangle by whether its stored normal faces the eye, a test made on the CPU and
independent of GX. At two poses, `GX_CULL_BACK` must keep only samples whose
stored normal faces the eye, and `GX_CULL_FRONT` must keep only samples whose
stored normal faces away. Each mode must keep at least one sample. The status
text overlay is drawn without culling.

## Controls (interactive cycle)

Stick: orbit. C-stick: zoom. B: reset. START or HOME: exit. START is honoured
after 240 frames, once the camera has been driven.

## Limits

- Winding is qualified for this one section only: BSP 12, material 3. Other
  sections, objects and other geometry kinds need their own check before
  they share the cull mode.
- Back-face culling does not change CPU FIFO submission, and Dolphin's timing
  shows no measurable render-cost change. GP fill savings on hardware are
  unmeasured.
- Lighting is still flat and two-sided (absolute Lambert term). It does not
  use the stored normals.
- The state and sound slots are placeholder reservations, not engine pools.
- Physical Wii is untested.
