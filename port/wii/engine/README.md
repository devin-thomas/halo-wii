# Wii engine build (HWI-015)

`ninja wii_engine` builds `build/wii/engine.dol`: the whole Halo engine, as
the native ports compile it, linked against explicit Wii implementations of
the platform services it calls. It is the first build that runs the shared
engine on the Wii. It loads a real map staged on the SD card and runs the
engine's own game tick with a player (HWI-015B), and draws the map's
environment through GX each frame ([the Wii rasterizer](../render/README.md),
HWI-016B). Its scope and limits are below; see the evidence for
[the platform runtime](../../../docs/wii/evidence/2026-10-10-platform-runtime.md) and
[the real map's tick](../../../docs/wii/evidence/2026-10-10-real-map-tick.md).

## What is compiled

| Part | Files | Flags |
|---|---|---|
| Engine | every unit of `port/linux/port.json` "game" and `port/linux/game` (499 units) | `ENGINE_CFLAGS` (ADR-018) plus the game's MSVC dialect, [`include/halo_wii_prefix.h`](include/halo_wii_prefix.h) and the generated semantics header |
| Wii replacements | `physical_memory_map_wii.c` for `source/cache/physical_memory_map.c` (fixed Xbox addresses, ADR-016); `cache_files_wii.c` for `source/cache/cache_files_windows.c` (the Xbox hard-disk cache: maps staged on the SD card instead, below); `source/memory/byte_swapping.c` from a copy with MSVC integer suffixes respelled (`tools/wii/engine_rewrite.py`) | engine |
| Engine-side driver | `engine_hooks.c`, `fixed_step_scenario.c`, `real_map_scenario.c` | engine |
| Shared run | `engine_map_run.c`: the real-map run the Wii driver and the i686 host reference both execute | platform |
| Reused Linux platform files | `xbox_files.c`, `xbox_xapi.c`, `msvc_crt.c` (with a PowerPC branch for `_control87`), `msvc_wide.c`, `halo_linker_common.c`, `port_config.c` | platform |
| Wii services | `wii_kernel.c` (handles, events, mutexes, threads, time, heap, debug output), `wii_memory.c` (ADR-015 MEM2 arena, contiguous memory), `wii_posix_files.c` (newlib/libfat), `wii_log.c`, `wii_sdl_files.c` | platform |
| Unsupported | `unsupported_sdk.c` (generated from the SDK prototypes, `tools/wii/engine_unsupported.py`), `unsupported_port.c` | platform |
| Third party | the ports' musl-derived maths, zlib, tomlc17 | as upstream |

Gates before the link (`tools/wii/build.py`): no engine object references a
newlib wide-character function (ADR-018), and no object holds thread-local
storage (libogc sets up no thread pointer; r2 is the EABI small-data base).

The link keeps the engine's own `main` (`source/shell/shell_xbox.c`) and
everything it reaches, so the image is the whole game's
(`tools/wii/engine_image.py` reports it from the map).

## Platform services

| Service | Wii implementation | Not provided |
|---|---|---|
| Time | PowerPC time base, microsecond performance counter; calendar from the RTC | — |
| Threads | newlib pthreads on libogc threads; each clears FPSCR[NI] first (ADR-018) | priorities are hints only |
| Files | Xbox paths translated below `sd:/halo-wii-engine/data` (`d:\`) and `.../saves/<drive>` (`z:\`, `u:\`, `t:\`) by the reused `xbox_files.c` | FAT keeps no access/creation times or permissions; `pread`/`pwrite` are a locked seek-transfer-seek |
| Memory | `wii_arena_open` places game state (20 MiB), tag cache (22 MiB), sound cache (4 MiB) and a 1 MiB contiguous region at the top of MEM2 at run time; `XPhysicalAlloc` serves the region | fixed physical addresses (fail); page protection is recorded, not enforced (logged `PARTIAL`) |
| Exit/restart | `XLaunchNewImage` exits (reused `xbox_xapi.c`); the halt path (`halt_and_catch_fire`) is reported to the log and exits | in-process re-initialisation of the engine (upstream keeps its game-state cursor across `game_state_dispose`) |
| Debug/asserts | `OutputDebugString` and `platform_log` to `sd:/halo-wii-engine/engine.log`; the engine's own `debug.txt` in `d:\` | — |

Everything else the engine links against reports itself: the first call of
each entry point logs `UNSUPPORTED <subsystem> <name>`, every call is
counted, and the call fails with its interface's failure value. Unsupported
now: Direct3D 8 (`render`), DirectSound (`dsound`), XInput and devices
(`input`), Winsock/XNet and internet play (`network`, `p2p`), Bink
(`bink`), the desktop hooks (`desktop`), the high-resolution HUD/text
(`hires_assets`), the PC menus (`menus`), the debug monitor (`xbdm`), the
Xbox hard-disk cache (`cache`), and Custom Edition maps' fixed tag window
(`custom_edition`).

## Diagnostic storage left out (HWI-015D)

By default (`configure.py --wii-diagnostic-storage omit`) the engine units
compile without two diagnostics that nothing in the game reads
([the audit](../../../docs/wii/evidence/2026-10-10-diagnostics-storage-audit.md)):

| Storage | Define | Left out | What remains |
|---|---|---|---|
| The AI's debug records | `HALO_AI_DEBUG_RECORDS=0` ([ai_debug.h](../../../source/ai/ai_debug.h)) | 545,252 bytes of `ai_debug` and 30,359,424 bytes of heap | `ai_debug`'s controls (debug selection, AI cheats, print/render switches); one shared 25,980-byte heap record that every actor's debug information is written to; no path debug storage |
| The profiler's frame history | `HALO_PROFILE_FRAME_HISTORY=0` ([profile.c](../../../source/cseries/profile.c)) | 1,124,352 bytes of `profile_globals` | the section profile (`profile_dump`, `profile_display`); the frame dump writes each frame as it ends; the profile graph draws nothing |

`ai_debug_initialize` then runs as upstream's (it sets the debug selection,
which player spawning reads, to none). `--wii-diagnostic-storage keep`
compiles both as the other ports do; `ai_debug_initialize` is then reported
unsupported (`diagnostics`, 30.36 MB of heap at the ports' capacities).
Other ports are unchanged: the defines default to 1.

## Maps on the SD card (HWI-015B)

The Xbox caches a map by copying it from the DVD into six cache files on its
hard disk (about 0.9 GB), which the Wii's card cannot hold. On the Wii a map
is prepared on the host at import time instead
([`tools/wii/map_stage.py`](../../../tools/wii/map_stage.py)), from the
owner's Xbox map, into `sd:/halo-wii-engine/data/maps/<name>.wmap`:

- **Byte order.** The engine reads tags in place as its own structures, so
  every scalar is converted to big-endian by explicit decoders, never by the
  host's layout: each tag root, block element and BSP root by its schema
  definition's full C layout
  ([`tools/wii/map_layouts.json`](../../../tools/wii/map_layouts.json),
  generated from the compiler's record layouts by `map_layouts.py`, refined by
  the schema's inline structures); the tag header, tag table, BSP header and
  buffer tables by their formats; each tag data field by what the engine reads
  it as (8- or 16-bit text, 32-bit bit vectors, BSP vertices, animation frames
  compressed and not, script nodes by type). Bytes no schema field claims
  (vertex and index payloads only the renderer reads) stay as they are.
- **Relocation (ADR-015).** The HWI-007 walker (`cache_schema_graph.c`) names
  every pointer field the game reads; each holds its target's offset in the
  tag slot, and the file lists them. The fields the walker nulls are zero.

`cache_files_wii.c` serves the engine's own `scenario_tags_load` and
`scenario_structure_bsp_load`: a staged map counts as precached, its tag
data and BSP are read at once into the tag slot, and each listed field gets
the slot's run-time base. The engine's own validator then checks the
result, as it checks every map. Bitmap pixels and sound samples are not
staged: their reads fail and report `UNSUPPORTED content`.

The texture and sound caches (made by the rasterizer's and sound's
start-up, which do not run) and what feeds them
(`predicted_resources_precache`) report themselves unsupported, as do the
decals' per-map vertex-cache flush (the decals' own unlocking is kept).

## What one launch does

1. Clears FPSCR[NI], records the memory map, starts video and GX as the
   diagnostics do, mounts the SD card, counts the launch.
2. Checks the services through the SDK interfaces (time, a thread and its
   FPSCR, events, a mutex, a file written and read back through
   `z:\` with different case, contiguous memory, debug output).
3. Opens the MEM2 arena, then runs the engine's start-up: `shell_initialize`'s
   steps (`tag_files_open` is the engine's, on `cache_files_wii.c`;
   `rasterizer_initialize` is reported unsupported: it halts the engine
   without Direct3D) and `game_initialize` (its decal vertex buffers reported
   unsupported, every game-state allocation recorded for the digests).
   Reports the diagnostic storage compiled and that `ai_debug_initialize` ran.
4. The real-map run ([engine_map_run.c](engine_map_run.c),
   [real_map_scenario.h](real_map_scenario.h)), which the i686 host reference
   runs too (`tools/wii/run_engine_map_host.py`):
   - the HWI-015A fixed-step scenario once at 30 Hz: its digest must still be
     `c1ad6ae6`;
   - an unmeasured warm-up run, then three cycles of six runs. Each run loads
     the map named by `map.txt` through the engine's own `game_load` and
     `game_initialize_for_new_map`, makes a local player, runs 300 of the
     engine's own ticks through its scheduler at one render cadence (60, 30,
     20 and 144 Hz, an irregular cadence with a 0.5 s hitch, frames paced by
     the vertical retrace) with input scripted per tick, and unloads the map.
   - After every tick it digests the whole game state, allocation by
     allocation, in a byte-order-independent form, and the simulation (the
     same without the three allocations the render frames reach). Every run's
     simulation digests must be the first run's; the heap must be flat
     across cycles.
5. Shuts the engine down; the arena's bound must be restored. Reports the
   main stack's peak, the engine threads' stack peak and every unsupported
   entry point called.
6. Triggers a fatal engine assertion last; its halt is reported and the
   program exits.

With `sd:/halo-wii-engine/full_game.txt` on the card, the engine's own
`main` runs instead and shows what the unmodified start-up does on the Wii.

## Files on the SD card

`sd:/halo-wii-engine/`: `engine.log` (the report), `runs.txt` (launch
counter), `result.txt` (last digests, compared at the next launch),
`config.toml` (the ports' settings, written by `port_config.c`),
`data/debug.txt` (the engine's log), `map.txt` (the staged map's scenario
path, `levels\...\<name>`), `data/maps/<name>.wmap` (the staged map,
private: never in Git).

## Limits

- Only the structure BSP's environment is drawn (port/wii/render, HWI-016B);
  the rest of the renderer, the texture and sound caches are unsupported,
  and bitmap pixels and sound samples are not staged.
- One structure BSP per map is staged (the multiplayer maps have one);
  campaign maps with several need a BSP switch in the stager.
- The `metr` tags (HUD meters, read only by the renderer) have no schema
  definition and stay unconverted.
- Emulator only. Physical Wii is untested.
