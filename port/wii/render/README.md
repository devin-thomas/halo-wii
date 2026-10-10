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

## What is drawn

- **Geometry:** every material of the BSP, in `structure_render_pass`'s
  lightmap and material order, whose breakable surface stands
  (`breakable_surface_extant`); of each, the surfaces
  `render.environment_surface_flags` marks visible.
- **Environment shaders:** the lightmap page times the base map, two TEV
  stages. Lightmap texcoords are the engine's `(2s + 1) / 65535` through the
  texture matrix.
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
self-illumination, mirrors, the HUD, interface, widgets, menus and text.
The camera effect matrix (impulses and shakes) is not applied: the engine's
`player_effect_get_camera_effect_matrix` writes the game state (player
effects), which a render path may not.

## Resources and memory

`tools/wii/render_stage.py` copies a map's files from one content pipeline
generation (private; never in Git) to the card:
`data/render/<map>/lightmaps/<bsp tag>.hwl` and
`data/render/<map>/textures/<bitmap tag>-<bitmap>.hwt`. The HWL1 decoded form
is reduced to positions, base texcoords and lightmap texcoords (24 bytes a
vertex) and released. Lightmap pages are always loaded; base maps are loaded
in order of the surfaces they cover while they fit `base_budget` (default
2 MiB, `wii_render.h`), pending the Wii texture cache (HWI-032). Base maps
left out are logged with their bytes (`RENDER_BASE_NOT_RESIDENT`).

## `render.txt`

Optional, on the card at `sd:/halo-wii-engine/render.txt`:
`render=off` (the HWI-015B build: nothing drawn, the rasterizer reported
unsupported), `mode=poses_only` (skip the map run; only the fixed-pose
checks, for short frame dumps), `base_budget=<bytes>`.
