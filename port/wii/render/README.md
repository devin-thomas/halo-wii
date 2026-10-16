# Wii rasterizer: the environment path (HWI-016B)

`engine.dol` draws the loaded map's structure BSP through GX every
`game_frame`, from the engine's own render camera and with the engine's own
visibility. This replaces the `rasterizer_initialize` the Wii build skipped
(HWI-015B) with what the environment path needs. HWI-082 adds the objects'
models over it (real geometry and poses, diagnostic material: below).
Everything else the renderer draws stays unsupported and reports itself.
[Evidence](../../../docs/wii/evidence/2026-10-10-engine-environment-render.md)

| File | Side | What |
|---|---|---|
| [`wii_render.h`](wii_render.h) | interface | C types only; the three parts below and `render.txt` |
| [`render_engine.c`](render_engine.c) | engine unit | the rasterizer's start-up and per-map hooks; each frame's camera (`director_update`, `observer_update`, the window camera of `set_window_camera_values`) and visibility (`structure_visibility_find_camera`, `render_camera_build_frustum`, `structure_visibility_compute`); what each BSP material is; the fixed-pose checks |
| [`render_gx.c`](render_gx.c) | libogc | converted geometry (HWL1) and textures (HWT1) from the card, GX state, the draw, EFB samples, the frame copy, timings and memory |
| [`render_texture_cache.c`](render_texture_cache.c) | strict C11 | HWI-083: where streamed base maps live in their arena, which may be evicted, and when the GPU no longer reads their bytes (tested on the host: `tools/wii/test_texture_cache.py`) |

The hooks are registered with `port/wii/engine/engine_hooks.c` by the Wii
driver. The i686 host reference (`tools/wii/run_engine_map_host.py`) builds
the same engine units without this directory and registers none, so it runs
the HWI-015B sequence unchanged: the comparison of the two shows whether
drawing changes the game state.

## The boot mode's interface (HWI-016D)

In the boot mode (port/wii/engine/README.md) the engine's own `main_loop`
draws, and the boot hooks route its rasterizer backend here:

| File | Side | What |
|---|---|---|
| [`wii_render_ui.h`](wii_render_ui.h) | interface | the boot configuration and the 2D GX calls |
| [`render_ui.c`](render_ui.c) | engine unit | `render_frame` for main_loop's windows (the environment from each window's own camera, then `render_ui_widgets`); the screen quads (each bitmap by its tag and index in its tag); the engine's own text and glyph cache; which screen is up (BOOT_SCREEN: EFB CRC and game-state digest once it has stood); the manifest line and the map's label; the end of the launch |
| [`render_ui_gx.c`](render_ui_gx.c) | libogc | 2D GX state, the engine's framebuffer blend functions, the menus' HWT1 textures from the card in a bounded cache (split-alpha CMPR with its I4/I8 plane on a second TEV stage), the glyph cache's A4R4G4B4 bitmap as an IA4 texture, a built-in 5x7 diagnostic font, the frame copy |

HWI-033 completes the interface's drawing:

- **Plasma:** the menus' plasma effect is composited as the Xbox's register
  combiners compute it (`rasterizer_xbox_dynavobgeom.c`, map0_to_1 = min with
  the plasma bit), as a TEV program of 12 stages (15 with alpha planes):
  `wii_ui_gx_plasma`. `tools/wii/ui_combiners.py` holds both programs as
  models; the TEV's 8-bit result is within 2 of the combiners' float result
  for 89% of inputs and within 8 for 99.5% (the rest lie on the program's
  own threshold, `y >= 1/2`, where a step of input precision flips it).
- **Min and max:** GX's blender has neither, and its logic operations are
  bitwise. The destination is copied from the EFB to a texture and compared
  per channel in three TEV compare stages; the result replaces the pixel.
  Exact on the RGB8 EFB.
- **Texture coordinates:** each map's, as the Xbox's screen-geometry vertex
  shader makes them: anchored to the screen or the vertex's, times the map's
  scale, plus its offset, times its texture scale.
- **Self-test:** `boot.txt` `selftest=1` draws authored plasma and min/max
  cases before the engine starts and logs each EFB result
  (`BOOT_UI_SELFTEST`; `ui_combiners.py check-log`).
- **A campaign map** loaded from the menus (a scenario of type solo) draws
  its environment through HWI-030's path, labelled as the campaign's
  environment only (objects, the sky, effects and the HUD are not drawn
  yet). If a BSP cannot be drawn, the frame says so under a panel
  (`CAMPAIGN ENVIRONMENT NOT DRAWN`, `BOOT_ENVIRONMENT_NOT_DRAWN` in the
  log) and the game runs on.

Still approximated, counted: other map combinations (none in the menus) and
meters. The console window, the HUD and the game engine's overlays are not
drawn in the boot mode. Textures are read from
`data/render/<map>/textures/<tag>-<bitmap>.hwt` (`tools/wii/boot_stage.py`).

## What is drawn

- **Geometry:** every material of the BSP, in `structure_render_pass`'s
  lightmap and material order, whose breakable surface stands
  (`breakable_surface_extant`); of each, the surfaces
  `render.environment_surface_flags` marks visible.
- **Environment shaders:** the lightmap page times the base map, two TEV
  stages. Lightmap texcoords are the engine's `(2s + 1) / 65535` through the
  texture matrix.
- **Base map alpha (HWI-008F):** a base map converted with
  `gx-splitalpha-v1` is CMPR colour plus an alpha plane (I4 or I8), loaded
  as a second texture object with the colour's wrap, filter and mip chain.
  Where the shader reads the base map's alpha (environment types blended
  and blended base specular, whose diffuse pass blends the detail maps by
  it and uses it as the specular mask), a third TEV stage samples the plane
  (`TEXMAP2`, the base texcoord) into the output alpha; an RGBA8 base map
  gives its own alpha. The detail maps and specular that consume it are
  HWI-032's. Each pose check also draws the base maps' combined alpha as
  grey (`crc_base_alpha`).
- **Culling:** `GX_CULL_BACK` with the stored corner order, as qualified in
  HWI-016A. Every pose check classifies the visible samples with the
  engine's `render_camera_triangle_frontfacing` under each cull mode.
- **Placeholders, labelled:** transparent shaders (generic, chicago, water,
  glass, meter, plasma) are a 50% magenta blend without depth writes, drawn
  after the opaque ones; any other non-environment type is flat yellow. An
  environment material whose base map is not resident draws lightmap-only.
  The clear colour stands where the sky would be.

## Object models (HWI-082)

Every frame, in the map run, the boot mode and each pose check, the objects
in the rendered clusters are drawn over the environment, inside the same GX
draw, as models: the engine's real geometry, node matrices and permutations,
with a diagnostic material.

| File | Side | What |
|---|---|---|
| [`render_objects_engine.c`](render_objects_engine.c) | engine unit | the objects of the rendered clusters (`structure_visibility_find_objects`, with marks of its own: the engine's object marker writes each object's `magic_number`); `render_object_list`'s walk (children too; not the local player's unit in first person); `render_model`'s detail cutoff, level of detail and region permutations; the parts of each pass; the node matrices (the object's, times each node's `runtime_default_inverse_matrix`) |
| [`render_models_gx.c`](render_models_gx.c) | libogc | each part skinned on the CPU from the tag slot into a bounded buffer and drawn as indexed strips or lists (`GX_VTXFMT3`), in three passes |
| [`../content/hwm_model.c`](../content/hwm_model.c) | both | `hwm_skin_decode_le` and `hwm_skin_apply`: the one vertex profile (below) |

**What is read.** Only the game state, as it stands: each object's node
matrices as the simulation computed them (with their scale and animation;
in the boot mode, the ports' interpolated pose), its region permutations,
flags, children and bounding sphere; the model and shader tags. Not used:
`object_get_cached_render_lighting` and `object_get_cached_render_state`
(they write `cached_render_state_index` and the cache), the camera effect
matrix (it writes player effects), the object marker. Each pose check's
game-state digest, taken around the visibility and every draw, covers the
models too.

**The vertex profile.** A model part's vertex buffer stays where the
staged map holds it: the tag slot's tag data, as the map holds it
(little-endian; `map_stage.py` converts only what the game reads), with
its index buffer (big-endian: model indices are converted). Its skinning
fields are decoded as the Xbox's vertex declaration for this vertex reads
them (`vertex_shader_declarations[0x78 / 4]`: PBYTE2 nodes, NORMSHORT1
weight) with the node matrices as `rasterizer_set_model_skinning` loads
them (three constant registers a node, every node of the model):
node = byte / 3, the first node's weight s16 / 32767, the second's 1 minus
it. `rasterizer_geometry_uncompress_vertices`' `(byte)weight / 255` is not
what the hardware reads. Under a full weight (32767) the second node's
weight is exactly 0 and its byte is not read. `tools/wii/model_vertex_probe.py`
measures a staged map against these rules (counts only; on a10: every first
node byte a triple below the model's nodes, every weight from 1560 to 32767,
every partially weighted vertex's second byte a triple below them too; the
fully weighted vertices' second bytes arbitrary; no part with local nodes).
A part with any vertex outside the profile, a vertex type other than 5,
buffers outside the tag data, an index past its vertices, or more vertices
than the skinning buffer holds, is refused whole and counted
(`RENDER_MODEL_PART_REFUSED`, the first 16 logged; `RENDER_MODEL_TOTALS`),
never drawn with another node.

**Memory and GX.** The skinning buffer (`model_skin_bytes`, default
512 KiB: 32,768 vertices of 16 bytes, position F32 and colour RGBA8) is
taken from the BSP's store after the material tables, before the textures
fill it, and given back at the BSP's end; without it the models are not
drawn (`RENDER_MODEL_SKIN_NOT_RESIDENT`). A frame fills it from its start; a
part that does not fit what is left waits for GX and starts it again
(`wraps`). Each draw ends with a draw sync token; the next draw and the
buffer's release wait for it unless GX has passed it.

**Material (diagnostic, labelled).** No texture, shader or lighting of the
engine's: one fixed directional light over the skinned normal. Solid parts
grey and opaque; the alpha-blended decals of model shaders light blue,
depth-tested without depth writes; transparent shaders' parts a 50% magenta
blend without depth writes, after every opaque part, in object order (not
sorted by distance). `GX_CULL_BACK` (the Xbox's `D3DCULL_CCW`, as for the
environment); two-sided model shaders draw both faces.

**Logs.** `RENDER_POSE` and `RENDER_RUN_POSE` add the model parts drawn,
refused and their triangles, and (poses) `object_samples`: the samples the
models changed over the environment's full draw. `RENDER_OBJECTS`: the
objects found (at most `MAXIMUM_RENDERED_OBJECTS`, the engine's), the
models drawn and what was skipped, after the first frame with objects, at
each pose phase's end and each map's end.

## What is not drawn (each reported `UNSUPPORTED render ...`)

Object models' shaders, textures and lighting (the material above),
transparent sorting, model effects (active camouflage, modifiers), change
colours, object shadows and widgets, first-person weapons (and their node
remapping), the sky, decals, particles,
contrails and weather, effects, lights, lens flares and shadows, water and
fog, environment detail maps, bump maps, specular, reflections and
self-illumination, mirrors and the HUD. The interface's widgets, menus and
text are drawn in the boot mode (above); the map run draws none of them and
reports them.
The camera effect matrix (impulses and shakes) is not applied: the engine's
`player_effect_get_camera_effect_matrix` writes the game state (player
effects), which a render path may not.

## Resources and memory

`tools/wii/render_stage.py` copies a map's files from one content pipeline
generation (private; never in Git) to the card:
`data/render/<map>/lightmaps/<bsp tag>.hwl` and
`data/render/<map>/textures/<bitmap tag>-<bitmap>.hwt`.

**Geometry (HWI-030)** costs nothing beyond the tag slot. GX draws each BSP
material's vertices where the staged map already holds them for the game:
the 32-byte compressed environment vertex (position F32 at 0, texcoord F32
at 24) and the 8-byte lightmap vertex (texcoord S16 at 4), big-endian as
`map_stage.py` stages them, indexed by the BSP's surfaces block. The HWL1
file holds the same bytes; at every BSP load it is read in 32 KiB chunks and
compared byte for byte (`RENDER_GEOMETRY_CHECK`), and a BSP that differs is
not drawn. HWI-016B's path decoded the whole file (normals expanded to F32)
beside it: a10's first BSP needed 8,100,736 bytes, its largest 11,664,352.

**Textures** fill one render pool per map, taken from the heap at the map's
first BSP load (MEM1's arena above `pool_keep`'s first value, MEM2's above
the 2 MiB reserve and the second) and given back when the map unloads. Each
BSP load, including a script's BSP switch, refills it from the start: the
material tables, every lightmap page the BSP uses, then its base maps in
order of the surfaces they cover while they fit `base_budget` (default
3 MiB, `wii_render.h`) and the pool, pending the texture cache (HWI-054).
Base maps left out are logged with their bytes and why
(`RENDER_BASE_NOT_RESIDENT reason=budget|pool`), resident ones with their
format and alpha (`RENDER_BASE_RESIDENT`). Their materials draw
lightmap-only, and every frame counts the visible triangles drawn textured
and lightmap-only (`RENDER_MAP_STATS`). The boot mode's menus share the
heap, so there each texture is its own heap block (HWI-016D's rule).

**Base map streaming (HWI-083, HWI-054; `texture_stream=1`, off by
default).** The baseline above leaves out whatever the budget does not hold
for the whole BSP: a10 has about 27.8 MB of base maps, of which 60–100% of
surface coverage is resident at the default budget. With
`texture_stream=1` the base maps live in a cache instead
([`render_texture_cache.c`](render_texture_cache.c); the cache allocates
nothing). At the BSP load, after the lightmap pages, one arena of at most
`base_budget` bytes (no more than the BSP's staged base maps) is taken from
the pool's free ends (MEM1's first, at most one region in each part) or, in
the boot mode, as one heap block. The pool still never takes the MEM2
reserve or the `pool_keep` headroom. The load fills the arena in coverage
order (the baseline's residency at the same budget). Before each frame's
draws, the base maps of the visible surfaces (the engine's surface flags
and drawn materials, counted per base map across every material that shares
it) are wanted. The most wanted map that is not resident is read into bytes
the cache gives. If needed, those bytes come from evicting maps that no
surface has shown for `stream_keep_frames` frames; a visible map is never
evicted. The map is then checked as the baseline checks it (HWT1 version 1
or 2 with its split alpha plane, the engine's dimensions, GX-loadable images
on 32 bytes), and only then published. A frame loads at most
`stream_loads` maps and `stream_frame_bytes` bytes (one larger map may load
alone). Maps left for a later frame draw lightmap-only, as before.

GX completion: every `draw_materials` is one draw sequence, and every base
map it binds records it. A sequence is complete once a `GX_DrawDone` issued
after it has returned: the map run's frames and the pose checks do this
themselves. The boot mode's frame completes in `render_ui_gx.c`, which this
side does not see. The cache calls `GX_DrawDone` itself before it hands out
bytes of a map whose sequence it has not seen complete, and
`wii_gx_map_end` does the same before any BSP memory is given back or
refilled. A load invalidates GX's texture cache before the frame's draws.
An evicted map's texture objects and pointer are cleared at once.

Refused for the BSP, and logged once: a map not staged (`missing`), one
larger than every arena region (`oversize`), and content that fails the
checks (`invalid`). A failed or short read is retried after 120 frames, at
most 3 reads. A reservation made before a BSP change can never be published
(the cache's generation). The fixed-pose checks settle first: every visible
map that fits is loaded and any map not visible may be evicted, so all of a
pose's draws see one residency. That residency can depend on the arena's
earlier placement (fragmentation).
Logs: `RENDER_STREAM_CONFIG`, `RENDER_STREAM_BEGIN` (arena regions, store,
missing and oversize counts), `RENDER_STREAM_OFF` (why the BSP fell back to
the baseline), `RENDER_STREAM_STATS` at the BSP's end (loads, bytes,
evictions, GPU waits, no-space, failed reads, invalid, stale, frames with a
visible map not resident, deferred loads, and the time spent outside the
frame timings). Not qualified on Dolphin or hardware yet. The reads are
synchronous on the main thread, so a load costs its SD read time in that
frame (the bounds above cap it); the frame rate under streaming is unmeasured.

## `render.txt`

Optional, on the card at `sd:/halo-wii-engine/render.txt` (full list in
`wii_render.h`): `render=off` (the HWI-015B build: nothing drawn, the
rasterizer reported unsupported), `mode=poses_only` (skip the map run; only
the fixed-pose checks, for short frame dumps), `base_budget=<bytes>`,
`base_limit=<count>` (at most that many base maps, in coverage order:
compares texture profiles at equal residency; with streaming, the load's
fill only), `pool_max`, `pool_keep`,
`texture_stream=1` (HWI-083 base map streaming within `base_budget`;
default 0, the baseline), `stream_loads=<n>` (maps a frame, default 2,
1–64), `stream_frame_bytes=<bytes>` (default 524288),
`stream_keep_frames=<n>` (default 30),
`geometry_check`, `models=0|1` (HWI-082: the object models; 0 draws the
environment alone), `model_skin_bytes=<bytes>` (their skinning buffer),
`run_pose_ticks=<t>,...` (the map run's sampled frames)
and `poses=<scenario> [ticks=...] [switch_bsp=<t>:<i>,...] [overrides=each]`
(a fixed-pose phase; with `overrides=each`, two labelled diagnostic cameras
inside the BSP loaded at each tick: above one of its largest floor
triangles in an open leaf, looking toward its centre).
