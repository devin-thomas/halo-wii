# A real map and the engine's own game tick on the Wii (HWI-015B)

The Wii engine build now loads a real Halo map from the SD card and runs the
engine's own `game_tick` and `game_frame` with a player. The player follows
scripted per-tick input: it walks, jumps, fires and throws a grenade.

In stock Wii-mode Dolphin, every tick's game state is identical to an i686
Linux host's running the same map and inputs. This holds for the whole game
state at every checkpoint and for the simulation after every tick. It holds
at six render cadences, over three load/run/unload cycles and two cold
launches, on two emulator hosts. Nothing is drawn: the rasterizer stays
unsupported. [Numeric evidence](2026-10-10-real-map-tick.json)

**Result:**

- **Map loaded:** the multiplayer map Prisoner (build 01.10.12.2276). It is
  staged at import time and read straight from the card into the MEM2 tag
  slot, so the Xbox hard-disk cache is no longer used. The engine's own
  validator accepts it with 0 corrections.
- **Real tick:** 300 ticks a run of the engine's `game_tick`/`game_frame` in
  its own 30 Hz scheduler, with a player spawned at tick 1.
- **Host comparison:** all 18 runs of each launch equal the i686 host:
  - the whole state's digest and chain;
  - every allocation's digest at 4 checkpoints (1,404 digests);
  - every tick's simulation digest (300);
  - the tag slot.
- **Stability:** three cycles keep the heap flat and restore the arena. Main
  stack peak 97,920 bytes. Two cold launches pass on each of two hosts: host
  exit `0x0`, persistence 1→2, no OS instability events.
- **Engine defects found and fixed:** four, all found by this comparison
  (below).

## Build

| Item | Value |
|---|---|
| Source | Clean `8e66aafa` (code `ece1b07c`, `14c1b4d3`, `dc503023`, `8e66aafa`) |
| Build ID | `4f7a46b3524d52e6` (diagnostic storage `omit`, HWI-015D) |
| Toolchain | devkitPPC GCC 16.1.0, binutils 2.46.0 |
| `engine.dol` | 4,054,496 bytes, SHA-256 `e7136537bd51353830132e3d2fe2931eb2fe0e0eb7e519af75e4e93bbe260d15` |
| Host reference | Debian clang 19.1.7, i686, the Linux build's game flags |

The build-only artifact package (`package_artifacts.py stage` and `inspect`,
as the CI workflow runs them) passes: 25 files, 0 problems.

## Map loading: staging at import, direct SD reads at run time

`tools/wii/map_stage.py` reads the owner's Xbox map on the host. It writes
`<map>.wmap`, big-endian for the Wii, and `<map>.le.wmap`, little-endian for
the host reference. Both are private and never committed.

The engine reads tags in place as its own structures, so every scalar it
reads is converted to big-endian. Explicit decoders do this; nothing relies
on host byte order or on any compiler's bit-field layout (ADR-018).

| What | How it is converted | Bytes |
|---|---|---:|
| Tag roots, block elements, the BSP root (84,607) | Each schema definition's full C layout from clang's record layouts (`map_layouts.json`, 349 definitions; 0 conflicting unions, 0 bit-fields), refined by the schema's inline structures | 2,451,176 |
| Tag header, tag table, BSP header, D3D buffer tables | Their fixed word formats | 63,604 |
| Animation frames and defaults (uncompressed and compressed: header, node headers, 16-bit keyframe indices and 6-byte quaternions, floats) | By each animation's node flags and compressed header | 1,619,084 |
| Script syntax data (data array header, 19,001 nodes) | By node type: a constant boolean's byte, a constant short's short, otherwise 32-bit | 380,076 |
| BSP compressed vertices | 32-bit fields; lightmap vertices as a word and two shorts | 368,288 |
| Model part triangle indices (130,456) | 16-bit. The validator's model-part check reads them. | 260,912 |
| 16-bit text, cluster bit vectors | 16-bit and 32-bit words | 44,716 |
| 8-bit data (sound mouth data, fonts, 8-bit strings, script strings) | Unchanged | 120,329 |

- **Coverage:** exactly the 5,047,273 bytes the HWI-007 walker claims are
  converted, plus the indices.
- **Left as they are (2,610,867 bytes):** vertex and index payloads that only
  the unsupported renderer reads, and 2 `metr` HUD tags with no schema
  definition.
- **Self-check:** the stager checks that each loaded range holds the same
  bytes in both orders, so the conversion only reorders bytes within scalars.

**Relocation (ADR-015).** The HWI-007 walker (`cache_schema_graph.c`, given an
additive visitor entry point) names every pointer field the game reads:
16,934 fields to relocate and 23,669 to null.

- The staged file stores each relocated field's target slot offset and lists
  the field.
- `port/wii/engine/cache_files_wii.c` replaces `cache_files_windows.c`. It
  reads the tag data and the BSP straight into the slot when the engine's own
  `scenario_tags_load` and `scenario_structure_bsp_load` ask for them, adds
  the slot's run-time base to each listed field, and then lets the engine's
  validator check the result.
- A staged map counts as precached.
- Bitmap pixels and sound samples are not staged.

**Determinism:** staging is byte-identical on Titan (WSL clang walker) and on
the Research Mac (Apple clang). Big-endian SHA-256 `017dea1f…`, little-endian
`a22e1580…`, 7,988,952 bytes each.

## The real tick

Each run does the following:
- loads the map through the engine's `game_load` and
  `game_initialize_for_new_map`;
- makes a local player;
- runs 300 ticks of the engine's own scheduler at one render cadence;
- unloads.

The input is a script indexed by update, one update a tick. It walks
forward, strafes, backs off, jumps at ticks 60, 140 and 200, fires during
ticks 90–109 and 210–229, throws a grenade at tick 160, crouches and reloads.
A real-map run replaces the local game's `update_client_local_ticks`, which
is called from `game_time.c` and so wrapped. Before each tick's update is
built, it hands the update server that tick's action. Every cadence therefore
gives every tick the same input.

The player spawns at tick 1 and ends 11 m from its spawn point. Firing makes
effects and particles (the particle array reached 392 entries by tick 150). The local random
seed is set at each run's start; the clock seeds it at start-up.

**Digests after every tick** (`port/wii/engine/real_map_scenario.c`). They
cover every game-state allocation (78, recorded as `game_initialize` makes
them) and the live bytes of each:
- a data array's elements below its high-water mark;
- the object pool's blocks;
- any other allocation's bytes.

Two transforms make the digest independent of byte order:
- each word that points into the game state or the tag slot first becomes
  its region and offset;
- each 8-byte group contributes the multiset of its bytes.

There are two digests:
- **Whole state:** everything.
- **Simulation:** without the allocations the render frames reach:
  - everything `game_frame` advances (particles, particle systems,
    contrails, widgets, game sound);
  - three more, measured: the scheduler's frame-time remainder; the player's
    screen effects (counted down by the ticks of a frame, and drawn on the
    local random numbers); the first-person view model (its idle pose timer
    draws on those local random numbers).

The tag slot's digest is its loaded bytes' multiset, runtime fields included.

## Results, two cold launches on each host

| Check | Titan launch 1 | Titan launch 2 | Research launch 1 | Research launch 2 |
|---|---|---|---|---|
| FPSCR at entry → engine | `0x4` → `0x0` | same | same | same |
| HWI-015A fixed-step scenario | `c1ad6ae6`/`58ce9719` | same | same | same |
| Map loads (all 18 logged) | 7,919,052 bytes, 16,934 relocations, 0 unstaged reads | same | same | same |
| Validator corrections | 0 | 0 | 0 | 0 |
| Arena placement offset | 0 | 4,096 | 0 | 4,096 |
| Simulation digest, 18 runs | all `127656ef` (chain `73904c7f`) | same | same | same |
| Whole-state digest, first run | `7e21eb42` (chain `303df087`) | same | same | same |
| Heap across cycles | flat (2,500,912) | flat (2,500,944) | flat | flat |
| Arena after shut-down | restored | restored | restored | restored |
| Persistence | none (first), counter 1 | previous digest equal, counter 2 | none, 1 | equal, 2 |
| Guest result (log `END`) | pass, 0 failures | pass | pass | pass |
| Host exit | `0x0`, 99.7 s | `0x0`, 99.5 s | `0x0`, 97.1 s | `0x0`, 96.8 s |
| OS stability | no instability events (Windows System 41/1001/6005/6006/6008, Dolphin application errors) | same | no instability events (the runner's macOS kernel-log and crash-report checks) | same |

Both hosts run stock Dolphin 2609: Titan on Windows, D3D; Research on macOS,
Vulkan. The batch runner records the guest as `not_evaluated`, because it has
no scenario checker for this DOL. The guest result above is the log's `END`
line plus the comparisons in this record.

The halt path is still checked last. The deliberate assertion is reported and
the program exits. Physical Wii is untested.

**Cadence invariance (every launch).** All six runs of a cycle give the same
simulation digest after every one of the 300 ticks:
- 60 Hz (600 frames), 30 Hz, 20 Hz (201 frames, 2 ticks in some), 144 Hz;
- irregular (224 frames, a 0.5 s hitch, 7 ticks in one frame);
- the video's vertical retrace with measured frame times.

So do all three cycles.

Only frame-coupled allocations differ between cadences:
- game time globals;
- game sound globals;
- first-person weapons;
- player effects;
- particles.

Between cycles at one cadence, only the first-person view model and the
particles differ. Both depend on how the per-frame presentation consumed the
local random numbers before.

## The i686 host reference, tick by tick

`tools/wii/run_engine_map_host.py` builds the same engine units, the same
Wii platform layer and the same run for i686. libogc's few services are
shimmed, and MEM2 is mapped at the Wii's addresses (arena placement included).

- It replays each launch's measured retrace frame times.
- `tools/wii/compare_map_runs.py` compares the logs run by run.

| Launch compared | Runs, whole state equal | Runs, simulation equal | Allocation checkpoint digests equal | Per-tick simulation digests equal | Tag slot equal |
|---|---:|---:|---:|---:|---|
| Titan 1 | 18/18 | 18/18 | 1,404/1,404 | 300/300 | yes |
| Titan 2 (placement +4 KiB) | 18/18 | 18/18 | 1,404/1,404 | 300/300 | yes |
| Research 1 | 18/18 | 18/18 | 1,404/1,404 | 300/300 | yes |
| Research 2 | 18/18 | 18/18 | 1,404/1,404 | 300/300 | yes |

## Divergences found and how each was resolved

Each was found by the per-allocation comparison and located by dumping the
allocation (`dump.txt` on the card, `HWI015B_DUMP` on the host). None is
hidden: each was fixed, or is decoded or classified explicitly.

| Found | Cause | Resolution |
|---|---|---|
| Halt in `material_effects.c` on the Wii only: `#-5212 is not a valid index in [#0,#32)` | A short (`collision.material_type`) read through a 32-bit slot, `*(long *)&…`. On the little-endian Xbox that is the short; on PowerPC it is the next field. | **Engine fix:** the short itself, the same value on every port. |
| Validator corrections on the Wii only (model parts' indices past their vertices) | Model part index payloads were left little-endian; the validator's check reads them | **Stager:** model indices are converted |
| `objects` and `particle` differing between Wii and host | The location's `bonus` word, never read, carried a stack slot's old bytes from `scenario_location_from_point`'s callers | **Engine fix:** `scenario_location_from_point` sets it |
| `particle` differing | `particle_new` copied an unset `node_index` for unattached particles (read only when attached) | **Engine fix:** stores none when unattached |
| `players globals` differing in one byte (`0x0F` against `0xF0`) | An in-memory bit-field union (`local_player_triggered_switch : 4`), allocated in each compiler's order (ADR-018) | **Digest:** decodes that unit low-field-first; the engine reads only the fields |
| Script data freed from the tag cache (halt after a map with scripts) | `hs_scenario_postprocess` swaps `hs_syntax_data` to the map's array; the "allocated" flag still freed it | **Engine fix:** `hs.c` frees the array it allocated |
| Runs differing after the first, in one process | A map leaves state for the next: a light's marker copies a never-reset BSS counter; the first map sets a game-sound flag; the local random seed comes from the clock | **Run:** an unmeasured warm-up run, the light marker reset at load, and the local seed set per run |
| Tag slot differing | Unaligned scalars and unconverted bytes defeat fixed byte groups, and the validator points empty names at its own static string | **Digest:** the slot's byte multiset, and that one address as one value |
| `irregular` and `vsync` runs differing from the host | The host's driver ran x87 arithmetic for the frame times, and the host has no video | **Host reference:** SSE arithmetic, and it replays the Wii's measured frame times |
| The player stood still | `--wrap` misses `update_server_next_update`'s call inside its own unit | **Run:** the input is handed over before each update is built (above) |
| Transient decals not made | With no texture cache, a decal's texture query answers none, and `decals.c` skips the decal (the engine's own path) | **Measured gap:** reported `UNSUPPORTED render:texture_cache_bitmap_get_hardware_format`; needs a Wii texture cache (HWI-032) |

## Memory

| Item | Bytes |
|---|---:|
| MEM1 image `0x80003f00..0x8144c640` (code 3,177,512; read-only 689,350; data 186,806; BSS 17,212,778) | 21,268,288 |
| MEM1 left after GX | 2,002,944 |
| MEM1 left after engine start-up | 921,600 |
| MEM2 heap growth | 0 (the heap stays in MEM1) |
| MEM2 plan: game state 20 MiB, tag slot 22 MiB, sound 4 MiB, general 1 MiB | 51,380,224 incl. 2 MiB reserve; 2,985,984 spare |
| Tag slot loaded (tags 7,034,316 + BSP 884,736 at slot offset 22,183,936) | 7,919,052 of 23,068,672 |
| Game state allocated at start-up | 17,986,420 |
| Heap in use after each cycle | 2,500,912 (flat) |

## Stacks

- **Main stack:** peak 97,920 bytes below `main`'s frame, of 131,072 (painted
  window 114,688, not exhausted).
- **Engine threads:** every engine thread's stack below its entry frame is now
  painted and read back when its routine returns (`wii_kernel.c`). The map run
  starts no engine thread: the Xbox cache reader and sound threads do not
  run. The one thread created, the platform check's, peaked at the 256-byte
  entry gap.

## Unsupported, reported explicitly (17 names)

- **render:** `rasterizer_initialize`, `rasterizer_decals_initialize`,
  `rasterizer_decals_dispose_from_old_map` (the vertex-cache flush; the
  decals' own unlocking is kept), `rasterizer_decals_dispose`,
  `halo_interpolation_enabled`, `texture_cache_open`/`close`,
  `predicted_resources_precache`, `texture_cache_bitmap_get_hardware_format`;
- **d3d8:** `D3DResource_Register`, `D3DResource_BlockUntilNotBusy` (the
  tag buffers' registration);
- **dsound:** `sound_cache_open`/`close`;
- **network:** `XNetStartup`, `XNetGetEthernetLinkStatus`;
- **bink:** `RADSetMemory`;
- **menus:** `halo_menus_load`;
- **hires_assets:** `hud_hires_asset_count`.

Page protection is still logged `PARTIAL`.

## Timings

| Step | Titan (Windows 11, WSL Debian) | Research (M5 Max, macOS 27) |
|---|---:|---:|
| Map staging | 2.8 s (7.7 s with the walker's WSL build) | 1.2 s (1.55 s with its build) |
| Wii engine build, clean | 63.0 s (earlier clean builds 68.7, 74.0, 71.7 s) | not built there |
| Host reference build, clean | 128.0 s (earlier 105.4–124.3 s) | — |
| Host reference, 19 runs | 11.0–11.5 s | — |
| Dolphin launch, host elapsed | 99.7 s, 99.5 s | 97.1 s, 96.8 s |
| Guest, map load | 323–328 ms | same |
| Guest, first 300-tick run at 60 Hz with every digest | 3,630 ms | same |

The guest times are emulated time.

## Limits

- **Nothing is drawn.** The rasterizer, the texture and sound caches, bitmap
  pixels and sound samples are not on the Wii yet. Without a texture cache,
  transient decals are not made (a game-state difference from the Xbox until
  HWI-032).
- **One map.** It is a multiplayer map with one BSP and no script nodes, so
  map scripts do not exercise the script runtime. Its big-endian use of
  "short in a long" puns (`hs_runtime.c`) stays to be checked with a map
  that has scripts.
- **Not exercised here:** campaign maps (several BSPs: the stager stages one),
  AI and vehicles.
- **No game engine:** the map runs without a game variant, like a campaign
  start.
- **Emulator only.** Physical Wii is untested.
