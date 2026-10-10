# The map's environment drawn through GX inside the engine (HWI-016B)

The Wii engine build now draws the loaded map's structure BSP through GX in
every `game_frame`. It uses the engine's own camera and the engine's own
visibility. This replaces the `rasterizer_initialize` that HWI-015B skipped
with a Wii rasterizer for the environment path (`port/wii/render`).

On the multiplayer map Prisoner, in stock Wii-mode Dolphin, the results are:
- lit, textured environment pixels at fixed poses;
- back-face culling that matches the engine's own facing rule;
- per-tick game state identical to the i686 host reference, which draws
  nothing.

This holds over two cold launches on each of two emulator hosts.
[Numeric evidence](2026-10-10-engine-environment-render.json)

**Result:**

- **Game state untouched by drawing:**
  - The simulation digest stays `127656ef` (chain `73904c7f`) in every run.
  - The per-tick host comparison passes in all four launches: 18/18 runs,
    1,404/1,404 allocation checkpoints, 300/300 ticks, whole state included.
  - Each pose check digests the whole game state before and after its
    draws. The digests are equal at all 5 poses of all 8 launches.
- **Drawn:**
  - every structure-BSP material in the engine's draw order;
  - each material's visible surfaces from the HWL1 geometry, culled with
    `GX_CULL_BACK`;
  - environment shaders as lightmap page × base map in two TEV stages;
  - transparent shaders as labelled placeholders.
- **Not drawn:** every other render subsystem stays unsupported and
  reports itself (below).
- **Memory shortage, escalated:** the full environment of this map needs
  9,628,276 bytes. Only 3,825,664 bytes are free above the reserve, so
  5,802,612 bytes are missing. 8 of the 10 base maps (7,340,544 bytes) are
  not resident and their materials draw lightmap-only, labelled. Details
  and alternatives are below.
- **Found and fixed:**
  - an engine stack-byte leak into the game state (the collision result's
    location bonus word);
  - two render-path writes to the game state, both kept out of the frame
    (below).

## Build

| Item | Value |
|---|---|
| Source | Clean `8437db55` (code `d06b5c6a`, `00b54543`, `8437db55` on `e12877b3`) |
| Build ID | `ac8738a5dc9b1e5a` |
| Toolchain | devkitPPC GCC 16.1.0, binutils 2.46.0 |
| `engine.dol` | 4,096,928 bytes, SHA-256 `25861b31295a2c04f844f1e0fbfbdc4ee61fdfe9f9b86874143a11c63c1cf981` |
| Host reference | Debian clang 19.1.7, i686, built from the same commit |
| Content | Generation `17189aa32aa83526`: 13 files staged by `tools/wii/render_stage.py` (private) |

The CI's build-only package passes (`package_artifacts.py stage` and
`inspect`): 33 files, 0 problems, 0 findings. Tests pass on Python 3.14 and
3.11: 438 tests.

## How a frame is drawn

The Wii driver registers the hooks with `engine_hooks.c`. The i686 host
reference registers none, so it runs exactly as in HWI-015B.

| Step | Engine code | Wii side |
|---|---|---|
| Start-up | `shell_initialize`'s rasterizer step: frame and screen bounds, clip distances (`rasterizer_frame_begin`'s defaults) | GX state, two vertex formats, lightmap texture matrix |
| Per map | after `rasterizer_initialize_for_new_map`, before `rasterizer_dispose_from_old_map`; each BSP material's shader type, base map and bitmap (`permutation % count`), lightmap page and breakable surface | HWL1 loaded, reduced to positions, base texcoords and lightmap texcoords, then released; lightmap pages; base maps by surface coverage within the budget |
| Camera | `director_camera_deterministic` for the local player's unit, made into the window camera as `set_window_camera_values` makes it, with `compute_window_bounds` | the frustum's `world_to_view` as the GX position matrix; its projection as `GX_PERSPECTIVE` |
| Visibility | `structure_visibility_find_camera`, `render_camera_build_frustum`, `structure_visibility_compute` (portals, cluster PVS, subcluster frustum tests) | the engine's surface bit vector |
| Draw | `structure_render_pass`'s lightmap and material order; `breakable_surface_extant` | indexed triangles of the visible surfaces; opaque first, then the transparent placeholders |

Prisoner has one cluster and no portals, so its PVS is trivial. Visibility
still culls through the subclusters' frustum tests: at the three engine
poses, 2,485, 707 and 388 of 5,234 surfaces are visible.

## Game state with drawing on

### Comparison with the i686 host

The Wii draws every frame. The host draws nothing and replays each launch's
measured retrace frame times.

| Launch compared | Runs, simulation equal | Runs, whole state equal | Allocation checkpoints equal | Per-tick simulation digests equal |
|---|---:|---:|---:|---:|
| Titan 1 | 18/18 | 18/18 | 1,404/1,404 | 300/300 |
| Titan 2 (placement +4 KiB) | 18/18 | 18/18 | 1,404/1,404 | 300/300 |
| Research 1 | 18/18 | 18/18 | 1,404/1,404 | 300/300 |
| Research 2 | 18/18 | 18/18 | 1,404/1,404 | 300/300 |

- **Every launch:** simulation `127656ef`/`73904c7f`; the heap is flat
  across cycles; the fixed-step scenario is still `c1ad6ae6`.
- **The pose phase:** its 30 Hz run, with every frame drawn and the
  captures between frames, also ends at simulation `127656ef`.

**The whole-state digest moved from `7e21eb42` to `6d88af44` on both
sides.** The cause is the collision fix (below), not drawing:
- A launch with `render=off`, which draws nothing, gives `127656ef` and
  `6d88af44` too.
- The host reference, built from the same commit, gives the same.
- Reference values that HWI-016C records against `7e21eb42` must move with
  this fix.

### Render-path writes found, and kept out of the frame

| Found | How | Resolution |
|---|---|---|
| **main_loop's camera update feeds the simulation.** `director_update` and `observer_update` move the observer. Its camera is read by aim assist (`aim_assist.c`), object activation (`players.c`, PVS from the observer's position), the sound environment (`scenario.c`) and the listener (`game_sound.c`). | A dev build that ran both updates in the frame hook: simulation `ed291540`, first divergent tick 2, in `scenario globals`, `object looping sounds`, `game sound globals` and `particle` | The frame's camera is the engine's read-only deterministic one, `director_camera_deterministic`. The observer is never moved, as in HWI-015B. **Open:** the real-map run and its host reference both omit this main_loop step. A run with the real camera needs it on both sides (HWI-016C). |
| **The camera effect matrix writes player effects.** `player_effect_get_camera_effect_matrix` decrements impulse and shake timers, clears their "just started" flags and steps scripted effects. | At each engine pose it was called on a snapshot, the game state digested, and the snapshot restored: writes at ticks 150 and 300, none at tick 30 | Not applied. Impulses and shakes are not shown, and 10,689 frames went without them. |
| **Stack bytes in a particle's location.** `collision_test_vector` and `collision_test_pill_new` leave the result locations' bonus word unset. A result in `point_physics_update`'s stack slot carried that slot's old bytes into the particle. Drawing changed what the stack held. | Wii and host dumps of `particle` at tick 150: one word differs (`0x4e28` against `0`), at 60 and 144 Hz and vsync | **Engine fix** (`d06b5c6a`): the bonus word is 0, as `scenario_location_from_point`'s has been since HWI-015B |
| `rasterizer_initialize` allocates the model ambient reflection tint in the game state | Source | Not allocated, as in HWI-015B. Allocating it would move every later allocation away from the host's. Its users test it for NULL. **Open:** allocate it on both sides together. |

## Fixed poses (EFB, every launch)

- **Engine poses:** three of the engine's camera during the scripted
  player's 30 Hz run, at ticks 30, 150 and 300.
- **Diagnostic overrides:** two cameras placed from the BSP's bounds,
  labelled `diagnostic_override` in the log. The engine's visibility runs
  from each.
- **Samples:** each pass is sampled on a 32×24 EFB grid (768 samples).

| Pose | Visible surfaces | Covered | Distinct colours | Changed by the base map | Changed by the lightmap | Facing, cull none (front/back) | Cull back | Cull front |
|---|---:|---:|---:|---:|---:|---|---|---|
| Engine, tick 30 | 2,485 | 768 | 567 | 392 | 768 | 766 / 2 | 768 / **0** | **0** / 547 |
| Engine, tick 150 | 707 | 768 | 628 | 354 | 768 | 768 / 0 | 768 / **0** | **0** / 71 |
| Engine, tick 300 | 388 | 768 | 636 | 313 | 768 | 768 / 0 | 768 / **0** | **0** / 98 |
| Override, high | 5,117 | 570 | 412 | 306 | 570 | 26 / 545 | 570 / **0** | **0** / 564 |
| Override, mid | 4,537 | 768 | 527 | 533 | 768 | 767 / 1 | 768 / **0** | **0** / 501 |

**What the columns mean:**
- "Changed by the base map" counts samples that differ between the full
  draw and a lightmap-only draw.
- "Changed by the lightmap" counts samples that differ between the full
  draw and a base-map-only draw.
- **Facing:** each visible triangle is coloured by the engine's own
  `render_camera_triangle_frontfacing` and drawn under each GX cull mode.

**Culling:** `GX_CULL_BACK` never shows a back face and `GX_CULL_FRONT`
never shows a front face. Culling is therefore qualified again, here with
the engine's camera matrices rather than the diagnostic viewer's. The high
override looks at the map from above its roof: without culling, 545 samples
are back faces of the outer shell.

## Determinism

| Check | Titan (D3D) | Research (Vulkan) |
|---|---|---|
| Pose CRCs, full, lightmap-only and base-only (5 poses × 3) | equal in all 4 launches (2 full, 2 poses-only) | equal in all 4 launches |
| Pose CRCs across hosts | differ | differ |
| Covered, changed-by and facing counts across hosts | equal | equal |
| Run CRCs (16×12 grid, first frame at or after ticks 30, 150, 300 of each of 20 loads) | 60 per launch, equal across launches | equal across launches |
| Frame dumps, poses-only launches (212 frames each) | 204/212 identical across launches | 204/212 identical |

- **Pose CRCs across hosts:** distinct colour counts differ by 1 to 9 per
  pose. The two backends filter and blend textures slightly differently.
  Geometry, coverage and culling agree.
- **Run CRCs:** the same game time gives the same CRC whatever the cadence.
  Six values cover all 60 samples: three ticks, each reached either on the
  tick or one tick later.
- **Frame dumps:** frames 4, 7, 8, 11, 12, 15, 16 and 19 differ between
  launch 1 and launch 2. They differ the same way on both hosts.
  - **Cause:** frames are presented without waiting for the retrace, and
    launch 2's start-up takes a different path (it reads the previous
    result), so the retrace catches different early frames.
  - **Pose frames:** they are held for retraces and are identical.

## Render cost (guest timebase, emulated)

These are Dolphin timings. Dolphin does not model GP cost, so no hardware
figure follows from them.

| Per frame, 10,689 frames | Mean µs | Max µs |
|---|---:|---:|
| Camera and visibility (CPU) | 105 | 185 |
| Draw submission (CPU) | 249 | 313 |
| `GX_DrawDone` wait | 1 | 2 |
| EFB copy and present | 1 | 3 |
| **Total** | **357** | **502** |
| Triangles drawn | 1,190 | 2,719 |

- **Run length, drawing on against off:** the 60 Hz run takes 3,894 ms
  instead of 3,630 ms, and the 144 Hz run 4,728 ms instead of 4,159 ms.
- **Retrace-paced run:** 597 frames either way.
- **Resource load:** 41 ms per map load, next to the map's own 327 ms.
- **A whole launch:** 118,345 ms against 96,118 ms with `render=off`. The
  difference includes the pose phase.

## Memory

| Item | Bytes |
|---|---:|
| Engine image (HWI-015B: 21,268,288) | 21,349,792 |
| MEM1 free after GX (HWI-015B: 2,002,944) | 1,921,024 |
| MEM1 free after engine start-up (HWI-015B: 921,600) | 839,680 |
| HWL1 load peak: file and decoded form together, both released | 1,123,680 |
| Geometry resident: positions, base and lightmap texcoords (24 B/vertex, 32-byte aligned) | 222,720 |
| Surfaces resident (u16 triangles) | 31,404 |
| Lightmap pages (2, RGB565 256×256) | 262,272 |
| Base maps resident (2 of 10, RGBA8) | 1,747,840 |
| Per-map tables | 23,496 |
| **Owned at peak** | **2,287,732** |
| Heap growth in MEM1 | 454,656 |
| Heap growth in MEM2 (it never reached MEM2 in HWI-015B) | 2,002,944 |
| MEM1 left unused once the heap moved to MEM2 | 348,160 |
| MEM2 spare above the 2 MiB reserve: before, after | 2,985,984; 983,040 |

Everything is released at each unload. The heap is flat across cycles, and
the arena is restored at shut-down.

### Shortage, escalated (no capability cut)

All 10 base maps of Prisoner's environment shaders are DXT3 or DXT5. The
converter makes them RGBA8, four times their source bytes, because GX has no
DXT3/DXT5 equivalent: 9,088,384 bytes.

| Need | Bytes |
|---|---:|
| Full residency: geometry 254,124 + lightmaps 262,272 + base maps 9,088,384 + tables 23,496 | 9,628,276 |
| Free above the reserve after start-up: MEM1 839,680 + MEM2 spare 2,985,984 | 3,825,664 |
| **Shortage** | **5,802,612** |

With the provisional 2 MiB base map budget, the 2 base maps resident cover
3,051 of the 5,086 environment surfaces. The 8 left out (7,340,544 bytes)
are each logged `RENDER_BASE_NOT_RESIDENT` with their bytes. Their 2,035
surfaces draw lightmap-only.

**Alternatives, for decision (none taken):**
1. **Convert DXT3/DXT5 base maps differently** (HWI-008C/HWI-032): CMPR
   colour, 1,136,192 bytes for all 10, plus an I4 alpha map where the alpha
   is used, another 1,136,192. This changes fidelity: CMPR interpolates the
   middle colours at 5/8 + 3/8.
2. **A Wii texture cache that streams by visibility** (HWI-032).
3. **Recover MEM1** (HWI-015C), or **trim the game-state reservation** to
   its measured use: +3.0 MB. Upstream capacities stay; this is the owner's
   decision.

## Two cold launches per host

| Check | Titan 1 | Titan 2 | Research 1 | Research 2 |
|---|---|---|---|---|
| Guest result (`END`) | pass, 0 failures | pass | pass | pass |
| Shell with the Wii rasterizer | 1 (expected 1) | same | same | same |
| Simulation / whole state | `127656ef` / `6d88af44` | same | same | same |
| Heap across cycles | flat | flat | flat | flat |
| Persistence | none, counter 1 | previous digest equal, counter 2 | none, 1 | equal, 2 |
| Host exit | `0x0`, 122.7 s | `0x0`, 122.3 s | `0x0`, 119.4 s | `0x0`, 119.4 s |
| OS stability | no instability events | same | no instability events (macOS checks) | same |

**Other launches:**
- **Poses-only:** two on each host, all guest pass.
  - Host exit: Titan `0x0`, 26.3 s and 25.8 s; Research `0x0`, 20.9 s
    twice.
- **`render=off`:** one launch on Titan; guest pass, host `0x0`, 99.7 s.

**A host fault, recorded:** in an earlier poses-only batch on Research
(build `7ae94d0b31c1f268`, commit `00b54543`), launch 2 of 2 ended with
host exit `0xfffffff5` after the guest's `END pass`. It did not recur in
the 6 later Research launches. Physical Wii is untested.

## Timings

| Step | Titan (Windows 11, WSL Debian) | Research (M5 Max, macOS) |
|---|---:|---:|
| Render staging (13 files) | 0.66 s | copied from Titan |
| Wii engine build, clean | 74.4 s (earlier 71.7 s) | not built there |
| Host reference build, clean | 125.7 s | — |
| Host reference, 19 runs | 11.1–13.5 s | — |
| Dolphin, two full launches (batch) | 122.7 s, 122.3 s (269 s) | 119.4 s, 119.4 s (251 s) |
| Dolphin, two poses-only launches (batch) | 26.3 s, 25.8 s (71 s) | 20.9 s, 20.9 s (53 s) |
| Dolphin, `render=off` launch (batch) | 99.7 s (122 s) | — |

## Visual review (private)

Three Titan frame dumps from the poses-only launches, and the Research
version of one of them, are in the private run folder. They are never
committed or published.
- **Engine camera, tick 300.** The scripted player's view down a corridor:
  - a textured, lightmapped wall (resident base map);
  - lit corridor surfaces drawn lightmap-only (base map not resident);
  - soft lightmap shading on the floor.
- **Diagnostic override, high.** The whole BSP from above a corner:
  - the outer shell's back faces are culled, so the interior floors,
    walkways and pillars show through the roof;
  - small magenta areas are the labelled transparent placeholders;
  - the clear colour stands where the sky would be.
- **Diagnostic override, mid.** Inside, at mid height across the map:
  - layered walkways with lightmap light pools;
  - textured pillars and walls;
  - magenta placeholder panels in the ceiling.

## Unsupported, reported explicitly

Each is reported as `UNSUPPORTED render ...`:
- objects and models;
- first-person weapons;
- the sky;
- decals;
- particles, particle systems, contrails and weather;
- effects, lights, lens flares and shadows;
- water and fog;
- environment detail maps, bump maps, specular, reflections and
  self-illumination;
- transparent environment shaders (the labelled placeholders);
- mirrors, the HUD, interface, widgets, menus and text;
- the camera effect matrix.

On Prisoner's BSP:
- 68 transparent-shader materials (148 surfaces) are placeholders;
- 9 environment materials have detail maps and 11 have bump maps, none of
  them drawn.

The HWI-015B entries remain, except `rasterizer_initialize`: the texture
and sound caches, decals' vertex buffers, Bink, network and menus.

## Limits and remaining work

- **Base maps:** residency is short by 5,802,612 bytes (above). The texture
  cache and residency belong to HWI-032.
- **One map:** one cluster and no portals, so portal traversal and a
  non-trivial PVS are not exercised. Campaign maps with several BSPs, and
  BSP switches, are untested (the stager stages one BSP).
- **Camera:**
  - the real observer camera, smoothing and the camera effect need
    main_loop's camera update in both the Wii run and its host reference;
  - field of view comes from the unmoved observer.
- **Material model:** basic base × lightmap only. The full shader matrix
  is HWI-032.
- **Fog:** not applied, and `z_far` is not limited by fog.
- **Emulator only:** physical Wii and hardware GP cost are untested
  (HWI-040).
