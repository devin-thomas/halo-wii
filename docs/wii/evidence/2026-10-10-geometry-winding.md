# Geometry winding and back-face culling (HWI-016)

The real-geometry diagnostic from HWI-016A drew BSP 12, material 3 (2,990
triangles) without culling, because the triangle winding was unqualified.
That winding is now qualified on the host and again in the guest EFB, and the
diagnostic renders with `GX_CULL_BACK`. The original corner order is kept; no
indices are reversed.
[Numeric evidence](2026-10-10-geometry-winding.json)

## Engine convention (source)

- `render_camera_triangle_frontfacing`, `source/render/render_cameras.c:629`:
  a triangle is front-facing when `(p1-p0)×(p2-p1)` points away from the camera.
  In other words, its corners run clockwise as seen by the viewer.
- The view space built in `render_camera_build_frustum` (lines 1212–1223:
  x = forward×up, y = x×forward, z = −forward) and the projection (lines 1404–1412)
  are right-handed and do not mirror.
- The Xbox environment passes cull counter-clockwise triangles (`D3DCULL_CCW`).
  Examples: `rasterizer_xbox_environment.c:648, 1074, 1369, 2112`.
- libogc documents GX as treating primitives "clockwise to the viewer" as
  front-facing (`GX_SetCullMode`). It is the same rule, so `GX_CULL_BACK`
  applies to the original corner order.

## Host qualification (read-only, counts only)

A private script decoded the stored packed vertex normal with the engine's
`uncompress_int32_to_real_vector3d` rule (`rasterizer_geometry.c:516`). It
compared that normal with the right-hand corner-order normal
`(p1-p0)×(p2-p0)`. Two runs produced byte-identical output.

| Measure | Result |
|---|---|
| Triangles whose stored normal opposes the corner-order normal | 2,990 / 2,990 |
| Of those, cosine below −0.9 | 2,986 |
| Median cosine | −1.0 |
| Corners whose stored normal agrees with the corner-order normal | 2 / 8,970 |
| Shared edges traversed in opposite directions (consistent) | 1,528 / 1,528; 0 inconsistent |
| Boundary / non-manifold edges | 5,914 / 0 |
| Connected pieces / closed pieces | 1,480 / 0 |

The stored normals face the lit side, so the visible side sees the corners
clockwise. That matches the engine convention, and the shared edges show the
winding is consistent wherever triangles connect. No piece is closed (vertices
are split per piece), so a centroid inside/outside test does not apply here.

## Guest qualification (EFB, every cycle)

At load, the guest decodes the stored normals of every triangle through
`cache_material_get_surface_vertices` and `cache_material_decode_packed_vector`,
then cross-checks the positions against the existing per-triangle check. Result:
0 along, 2,990 opposed, 0 unclear.

Each cycle then draws a classified pass. The CPU colours every triangle by
whether its stored normal faces the eye: lit, unlit, or unclear/edge-on. That
choice is independent of GX. The pass is drawn under each GX cull mode at two
poses: pose 0 is the historical fixed pose from above, and pose 1 is from the
opposite side, slightly below. The figures are 32×24 grid samples, identical in
all four cycles of both launches:

| GX mode | Pose 0 lit / unlit / unclear | Pose 1 lit / unlit / unclear |
|---|---|---|
| `GX_CULL_NONE` | 30 / 33 / 0 | 22 / 34 / 0 |
| `GX_CULL_BACK` (content) | **47 / 0 / 0** | **44 / 0 / 0** |
| `GX_CULL_FRONT` (opposite) | 0 / 52 / 0 | 0 / 48 / 0 |

`GX_CULL_BACK` keeps only surfaces whose lit side faces the viewer, and
`GX_CULL_FRONT` keeps only their backs. The lit counts rise above the
no-culling counts because removing nearer back faces uncovers lit surfaces
behind them.

With the normal flat shading at pose 0:

| GX mode | Samples on geometry | Samples equal to no-culling | Grid CRC |
|---|---|---|---|
| `GX_CULL_NONE` | 63 | 768 | `aac744a4` (unchanged from HWI-016A) |
| `GX_CULL_BACK` | 47 | 740 | `18b17d9a` |
| `GX_CULL_FRONT` | 52 | 745 | `b9cf71f5` |

The section is interior environment geometry seen from outside. From these
poses, culling therefore removes the outward backs of walls, as it should, and
coverage drops. The discriminating result is the classified table above, not
coverage parity.

## Build and two cold launches

Clean source `5b009a82`, build `2024f5af91d4f65a`, devkitPPC GCC 16.1.0.
`geometry_view.dol` is 547,232 bytes, SHA-256
`38febd31f47913f8cc380feafd37a138a0e6c4a5e94c8e1bc44e1b14200ed79a`. The
Dolphin build, profile settings, staging and authored DTM
(`f66418ae…ebc5`) are unchanged from HWI-016A.

| Check | Each launch |
|---|---|
| Load/use/unload cycles | 4, each with CRCs `bad05748` / `db11989c` |
| Cull qualification | 4/4 cycles; every cull sample identical to cycle 0 |
| Released views and handles | 4/4 reject |
| Malformed manifests | 3/3 reject |
| Flipped BSP CRC, short tag length | 2/2 reject; MEM2 restored |
| Heap after load vs end | 1,669,488 bytes both |
| MEM2 plan | 50,579,712 of 54,284,128 bytes (unchanged; no new slots) |
| Interactive input | Orbit, zoom, two resets, exit by START |
| Launch counter | 1, then 2 |
| Frame dumps | 782 per launch; byte-identical across launches |
| Guest result | pass, both launches |
| Host exit | 0x0 natural, both launches; not forced |
| OS events | None in the System or Application logs during the run window |

The OS-events row covers System bugcheck/unexpected-shutdown events and
Application faults naming Dolphin. It is a session-scoped observation only.

## Cost change

Dolphin timings follow, and say nothing about hardware GP cost.

- **Frame render.** Rendering plus `GX_DrawDone` peaks at 305 µs per frame, the
  same as HWI-016A.
- **Fixed-pose draws.** A shaded draw takes 280 µs under every cull mode. A
  classified draw takes about 2,227 µs under every mode; that pass computes
  facing per triangle on the CPU and runs only in verification.
- **FIFO submission.** The CPU FIFO peak is 77,312 bytes per frame, against
  77,056 before (+256 B). Culling does not change the submitted vertex stream.
  The difference comes from the overlay (a new build ID, different status
  digits, an added cull-mode write) and was not isolated further.
- **Load.** Load time rises from 1.63 s to 1.70 s, because of the per-triangle
  stored-normal decode.

## Development iteration (retained)

`dev-1` ran from the uncommitted tree as build `02df8049f1720f07`. It produced
the same cull figures and CRCs and passed. Only its build identity differs from
the final run.

## Not established

- Winding for any other section, BSP, object or geometry kind. Each needs its
  own check before it shares the cull mode.
- Hardware GP fill savings, and physical Wii behaviour.
- Lighting from the stored normals. Shading stays flat and two-sided.
- Halo shaders, textures, lightmaps, collision and gameplay.
