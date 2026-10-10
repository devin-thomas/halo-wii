# Wii engine build (HWI-015)

`ninja wii_engine` builds `build/wii/engine.dol`: the whole Halo engine, as
the native ports compile it, linked against explicit Wii implementations of
the platform services it calls. It is the first build that runs the shared
engine on the Wii. It loads no map and draws nothing. Its scope and limits
are below; see [the evidence](../../../docs/wii/evidence/2026-10-10-platform-runtime.md).

## What is compiled

| Part | Files | Flags |
|---|---|---|
| Engine | every unit of `port/linux/port.json` "game" and `port/linux/game` (499 units) | `ENGINE_CFLAGS` (ADR-018) plus the game's MSVC dialect, [`include/halo_wii_prefix.h`](include/halo_wii_prefix.h) and the generated semantics header |
| Wii replacements | `physical_memory_map_wii.c` for `source/cache/physical_memory_map.c` (fixed Xbox addresses, ADR-016); `source/memory/byte_swapping.c` from a copy with MSVC integer suffixes respelled (`tools/wii/engine_rewrite.py`) | engine |
| Engine-side driver | `engine_hooks.c`, `fixed_step_scenario.c` | engine |
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
Xbox hard-disk cache (`cache`), Custom Edition maps' fixed tag window
(`custom_edition`), and the AI's debug records (`diagnostics`, 30.36 MB of
heap at the ports' capacities).

## What one launch does

1. Clears FPSCR[NI], records the memory map, starts video and GX as the
   diagnostics do, mounts the SD card, counts the launch.
2. Checks the services through the SDK interfaces (time, a thread and its
   FPSCR, events, a mutex, a file written and read back through
   `z:\` with different case, contiguous memory, debug output).
3. Opens the MEM2 arena, then runs the engine's start-up: `shell_initialize`'s
   steps (with `tag_files_open` and `rasterizer_initialize` reported
   unsupported: each halts the engine on the Wii) and `game_initialize`
   (its decal vertex buffers and AI debug records reported unsupported).
4. Three load/run/unload cycles of the controlled fixed-step scenario
   ([fixed_step_scenario.h](fixed_step_scenario.h)) at six render cadences
   each: 60, 30, 20 and 144 Hz, an irregular cadence with a 0.5 s hitch, and
   frames paced by the vertical retrace with measured frame times. The
   engine's own `game_time_update` schedules the 30 Hz ticks; every run must
   produce the same state digests, which equal the i686 host's
   (`tools/wii/run_engine_scenario_host.py`).
5. Shuts the engine down; the arena's bound must be restored and the heap
   flat across cycles. Reports the main stack's peak and every unsupported
   entry point called.
6. Triggers a fatal engine assertion last; its halt is reported and the
   program exits.

With `sd:/halo-wii-engine/full_game.txt` on the card, the engine's own
`main` runs instead and shows what the unmodified start-up does on the Wii.

## Files on the SD card

`sd:/halo-wii-engine/`: `engine.log` (the report), `runs.txt` (launch
counter), `result.txt` (last digests, compared at the next launch),
`config.toml` (the ports' settings, written by `port_config.c`),
`data/debug.txt` (the engine's log), `data/maps/` (empty: no map is staged).

## Limits

- No map loads. The engine reads tags in place, and Xbox tags are
  little-endian: a loaded map needs tags converted to the PowerPC's byte
  order (beyond ADR-015's relocation), and a Wii replacement for the Xbox
  hard-disk cache (`cache_files_windows.c`).
- The scenario is not the game's tick. It runs inside the engine's scheduler
  on engine state, data arrays, random numbers and maths; the game's own
  `game_tick` and `game_frame` need a map.
- Emulator only. Physical Wii is untested.
