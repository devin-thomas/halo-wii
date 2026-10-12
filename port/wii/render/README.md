# Wii rasterizer: the environment path (HWI-016B)

`engine.dol` draws the loaded map's structure BSP through GX every
`game_frame`, from the engine's own render camera and with the engine's own
visibility. This replaces the `rasterizer_initialize` the Wii build skipped
(HWI-015B) with what the environment path needs. Everything else the
renderer draws stays unsupported and reports itself (below).
[Evidence](../../../docs/wii/evidence/2026-10-10-engine-environment-render.md)

| File | Side | What |
|---|---|---|
| [`wii_render.h`](wii_render.h) | interface | C types only; the three parts below and `render.txt` |
| [`render_engine.c`](render_engine.c) | engine unit | the rasterizer's start-up and per-map hooks; each frame's camera (`director_update`, `observer_update`, the window camera of `set_window_camera_values`) and visibility (`structure_visibility_find_camera`, `render_camera_build_frustum`, `structure_visibility_compute`); what each BSP material is; the fixed-pose checks |
| [`render_gx.c`](render_gx.c) | libogc | converted geometry (HWL1) and textures (HWT1) from the card, GX state, the draw, EFB samples, the frame copy, timings and memory |

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
- **A campaign map** loaded from the menus is not drawn until its BSPs fit
  (HWI-030): its windows stay clear under a labelled panel
  (`CAMPAIGN ENVIRONMENT DRAWING PENDING (HWI-030)`), and nothing is loaded
  for it.

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

## What is not drawn (each reported `UNSUPPORTED render ...`)

Objects and models, first-person weapons, the sky, decals, particles,
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

## `render.txt`

Optional, on the card at `sd:/halo-wii-engine/render.txt` (full list in
`wii_render.h`): `render=off` (the HWI-015B build: nothing drawn, the
rasterizer reported unsupported), `mode=poses_only` (skip the map run; only
the fixed-pose checks, for short frame dumps), `base_budget=<bytes>`,
`base_limit=<count>` (at most that many base maps, in coverage order:
compares texture profiles at equal residency), `pool_max`, `pool_keep`,
`geometry_check`, `run_pose_ticks=<t>,...` (the map run's sampled frames)
and `poses=<scenario> [ticks=...] [switch_bsp=<t>:<i>,...] [overrides=each]`
(a fixed-pose phase; with `overrides=each`, two labelled diagnostic cameras
inside the BSP loaded at each tick: above one of its largest floor
triangles in an open leaf, looking toward its centre).
