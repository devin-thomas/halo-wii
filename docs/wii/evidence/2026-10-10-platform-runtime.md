# Platform runtime: the whole engine on Wii services (HWI-015)

The shared Halo engine runs on the Wii for the first time, through explicit
Wii implementations of the platform services it calls. In stock Wii-mode
Dolphin it starts, initializes its game, and runs a controlled 30 Hz
scenario in its own scheduler. The scenario's state is identical at six render
cadences, across three load/run/unload cycles and two cold launches, and
equal to an i686 host's. No map is loaded and nothing is drawn.
[Numeric evidence](2026-10-10-platform-runtime.json)

**Result:**

- **Compiled and linked:** the whole engine, 499 upstream units, links for
  PowerPC with the ADR-018 engine flags. The image is 22,975,648 bytes of
  MEM1, and every section, BSS included, lies in MEM1.
- **Runs:** the engine starts on the Wii and runs the fixed-step scenario.
  Unsupported subsystems report themselves.
- **Not loaded:** a map. Its tags are little-endian, and the Xbox hard-disk
  cache has no Wii replacement yet.
- **Memory:** the MEM1 budget is measured. Freeing room needs scope
  decisions (listed below).

## Build

| Item | Value |
|---|---|
| Source | Clean `cefa2090` (engine build `cdec0591`, base `b570b933`) |
| Build ID | `90e59e3b0113357e` |
| Toolchain | devkitPPC GCC 16.1.0, binutils 2.46.0 |
| `engine.dol` | 4,101,856 bytes, SHA-256 `317a15b3ab7c8dd265bbd9500fae2cb904cf3d952efd0d8a6f9a597a201e3f9d` |
| `engine.elf` / `engine.map` | 37,383,328 / 8,949,066 bytes (hashes in the JSON) |

`ninja wii_engine` ([port/wii/engine](../../../port/wii/engine/README.md)) builds:

- **Engine units:** every engine unit the native ports compile, plus two
  authored engine-side units, with `ENGINE_CFLAGS` plus the game's MSVC
  dialect.
- **Platform units:** 6 reused Linux platform files and 11 Wii platform
  units.
- **Third-party units:** 38.

Two units are replaced or rewritten:

- `physical_memory_map.c` is replaced, because it uses fixed Xbox addresses.
- `byte_swapping.c` is compiled from a copy with the MSVC `ui64` suffixes
  respelled.

Two gates pass before the link: no newlib wide-character references
(ADR-018), and no thread-local storage. Upstream source is otherwise
unchanged, except for one PowerPC branch in `msvc_crt.c` (`_control87` on
FPSCR).

## Two cold launches (stock Dolphin `1ed5e3f6…0dac`, Wii mode)

| Check | Launch 1 | Launch 2 |
|---|---|---|
| FPSCR at entry → engine | `0x4` → `0x0` | `0x4` → `0x0` |
| Platform checks (time, thread + its FPSCR, event, mutex, SD file via `z:\` in other case, contiguous memory, debug output) | 8/8 pass | 8/8 pass |
| Engine start-up | shell steps run; `game_initialize` completes; 17,986,420 bytes of game state allocated | same |
| Engine warning → `d:\debug.txt` (read back by the host) | present | present (both launches') |
| Cadence runs (3 cycles × 6 cadences) | 18/18 identical | 18/18 identical |
| Heap across cycles | flat (`arena2_lo` `0x900c6000` each cycle) | flat |
| MEM2 arena bound after shut-down | restored (`0x933db7e0`) | restored |
| Previous launch's digest (persisted) | none (first) | equal (`c1ad6ae6`/`58ce9719`) |
| Launch counter read back | 1 | 2 |
| Halt path (deliberate fatal assertion, last) | reported, exit | reported, exit |
| Guest result (log `END`) | pass, 0 failures | pass, 0 failures |
| Host exit | `0x0`, natural, 35.3 s | `0x0`, natural, 34.7 s |

The second launch placed the arena 4 KiB lower. No System event 41, 1001,
6005, 6006 or 6008 and no Application error naming Dolphin was logged during
the batch. The runner records the guest as `not_evaluated` because it has no
engine scenario checker. The guest result above is the log's own `END` line
plus the comparisons in this record.

## Fixed-step cadence invariance

The engine's own `game_time_update` (`source/game/game_time.c`) schedules the
30 Hz ticks. While the scenario is armed, the wrapped `game_tick` runs the
scenario's tick. That tick uses engine code: a data array in the game state,
the engine's random numbers, its vector maths, and the ports' shared
transcendental functions. The scenario also supplies a scripted per-tick
input. The digest is a CRC-32 of a little-endian serialization of the
scenario's state.

| Cadence | Frames | Frames without a tick | Most ticks in a frame | Digest after tick 300 | Chain of 300 digests |
|---|---:|---:|---:|---|---|
| 60 Hz | 600 | 300 | 1 | `c1ad6ae6` | `58ce9719` |
| 30 Hz | 300 | 0 | 1 | `c1ad6ae6` | `58ce9719` |
| 20 Hz | 201 | 0 | 2 | `c1ad6ae6` | `58ce9719` |
| 144 Hz | 1441 | 1141 | 1 | `c1ad6ae6` | `58ce9719` |
| Irregular (4–83 ms, one 0.5 s hitch) | 224 | 28 | 7 | `c1ad6ae6` | `58ce9719` |
| Vertical retrace, measured frame times | 600 | 300 | 1 | `c1ad6ae6` | `58ce9719` |

Checkpoint digests after ticks 1, 30 and 150 are equal too. The final state
has 33 live bodies, 72 spawned and 43 deleted, and the global random seed is
`56096e69`.

The irregular cadence's hitch exceeds a local game's limit of 7 ticks per
frame, so the engine discards the rest of that frame's time. The tick
sequence is unchanged; only the wall-clock mapping differs.

**i686 host reference:** Debian clang 19.1.7 with the Linux build's game
flags, the same engine units and the same scenario. It gives the same
digest, chain, checkpoints, seed and frame counts at every cadence (60 Hz
stands in for the retrace).

## MEM1: the actual PowerPC image

Measured from the linker map (`tools/wii/engine_image.py`). The link keeps
the engine's own `main` and everything it reaches.

| Section | Bytes | i686 estimate (ADR-015 record) |
|---|---:|---:|
| Code | 3,212,604 | 2,688,160 |
| Read-only data | 701,584 | 692,476 |
| Data | 186,806 | 394,823 |
| BSS | 18,872,810 | about 18.57 MB |
| **Image** `0x80003f00..0x815ed3a0` | **22,975,648** | |

| MEM1 after the image | Bytes |
|---|---:|
| Left by the image | 2,174,048 |
| Arena at `main` (libogc takes 65,536) | 2,108,512 |
| After the GX FIFO (256 KiB) and two framebuffers (614,400 each) | 294,912 |
| After engine start-up | 16,384; the heap then continues in MEM2 (802,816 bytes during the cycles, 856,064 at shut-down) |

**Named storage (BSS bytes):**

| Group | Bytes |
|---|---:|
| Custom Edition map support (`custom_edition_maps_globals` alone: 6,557,772) | 6,623,572 |
| Netcode and player input queues | 5,096,171 |
| Tag validator (`tag_validate_claims` 3,014,656) | 3,014,772 |
| Profiling (`profile_globals` 1,129,808) | 1,138,742 |
| AI debug, static | 547,730 |
| Renderer `render` globals | 643,732 |

The AI's debug records also need heap: 26,603,520 + 3,755,904 bytes at the
ports' capacities (1024 actors, 32 paths). That is more than both banks hold
free, so they are reported unsupported and not allocated.

**Placement decisions in this slice:**

| Decision | Effect |
|---|---|
| ADR-015 plan at run-time MEM2 bases | Game state 20 MiB, tag cache 22 MiB, sound cache 4 MiB and a 1 MiB contiguous region sit at the top of MEM2, with a 2 MiB reserve below. They are no longer fixed Xbox addresses, which the upstream `game_state.c` call also used. |
| Unwind tables dropped | C code without exceptions never reads them (the stack walker is the unsupported Windows one). About 0.44 MB of `.eh_frame` left the image. No gameplay effect. |
| No static storage moved between banks | The MEM2 room left after the plan and reserve is 2,985,984 bytes, and the heap already uses 0.86 MB of it on this path. No large group fits except profiling, and moving storage between the banks only moves the shortage. The combined free memory after GX and start-up is about 2.1 MB. |
| No capability cut | All upstream capacities are unchanged. |

**Scope decisions needed (for Devin), with exact bytes.** Each frees memory
only by changing what the build can do or carries:

1. Custom Edition map support: 6,623,572 bytes of BSS (the catalogue of up
   to 8,192 maps and 1,024 campaigns). It cannot run on the Wii anyway until
   its tags are relocated (no fixed `0x40440000` window).
2. Netcode and session buffers: 5,096,171 bytes (inventories, late-joiner
   players, damage trails and distributed state). These are needed for the
   required LAN and cross-port play.
3. The tag validator's claims bitmap: 3,014,656 bytes. It is used when maps
   load and could become a load-time allocation.
4. Profiling: 1,138,742 bytes. This is diagnostics only and could be
   compiled out if it has no gameplay effect (it records timings for
   display), but that has not been audited.
5. AI debug records: 547,730 bytes static plus 30,359,424 bytes of heap.
   These are diagnostics, but the AI writes them unconditionally. A write
   audit is needed before they can be made optional.

## Structure layouts on PowerPC against i686

- **Game state** (`tools/wii/engine_layout.py game-state`): all 75
  allocation rows of the HWI-007 census have the same count and element
  size on PowerPC as on i686, 17,804,952 bytes each. `struct data_array` is
  56 and `struct memory_pool` is 56.
- **Debug information** (`engine_layout.py dwarf`, unit by unit, the same
  499 units compiled for both): 10,103 unit-and-type pairs (1,472 distinct
  types) appear in both.
  - 10,099 are identical.
  - 4 differ, all C-library types (`struct tm` in 3 units and `struct
    _stat`: newlib against glibc). They are never engine data.
  - No engine type differs in size or member offsets.
  - Limit: DWARF numbers bit-fields in each target's own order, so this
    census does not show bit-field memory order. That is ADR-018's known
    difference, handled by explicit decoders at external boundaries.
- **Static assertions:** the engine's own compile-time size and offset
  assertions (1,452 typedef names in the sources) compile on PowerPC with the
  units that hold them.

## Unsupported, reported explicitly

On the scenario path, 9 entry points were called (15,509 calls; nearly all
are the per-frame interpolation query):

- `cache:tag_files_open`;
- `render:rasterizer_initialize`, `render:rasterizer_decals_initialize`,
  `render:rasterizer_decals_dispose`, `render:halo_interpolation_enabled`;
- `diagnostics:ai_debug_initialize`;
- `network:XNetGetEthernetLinkStatus`, `network:XNetStartup`;
- `bink:RADSetMemory`.

Each reports `UNSUPPORTED` once and fails its call. Page protection is
logged `PARTIAL`: it is recorded but not enforced, because the Wii build maps
no pages.

## Full-game mode (the engine's own `main`), preserved failure

With `full_game.txt` on the card, the unmodified start-up runs:

1. The debug-monitor walk reports an empty list.
2. `Direct3DCreate8` is reported unsupported.
3. The Xbox hard-disk cache setup begins creating about 0.9 GB of cache
   files on the 128 MiB card.

| Attempt | Outcome |
|---|---|
| Save root unset | Halted after 5 s, "setup for new cache file failed". The log was cut at 512 bytes, so the log is now synced after every line. |
| Final, save root set | Ran until the 240 s forced stop (host `0x1`, forced stop, not a natural exit). |

This is the expected unsupported path, not a pass: the cache layer needs a
Wii replacement.

## Known-good probe path

`probe.dol` from the same build (`72b3928f…`) passes its scenario in one
launch: Start pressed, storage sentinel 1, ABI lines present. Host exit was
`0x0` after 6.6 s.

## Tests

The Python unit tests pass: 302 tests on Python 3.14 and 3.11
(`python -m unittest discover -s tools/wii`). One run under 3.14 reported 2
failures right after a build. It did not reproduce in four reruns, and its
failure output was not captured.
`tools/test_wii_build.py` fails the same 2 tests before and after this change
in this worktree (one of them because a worktree's `.git` is a file); it was not run elsewhere.

## Limits

- No map loads. The engine reads tags in place, and Xbox tags are
  little-endian. A loaded map needs:
  - its tags converted to the PowerPC's byte order (beyond ADR-015's
    relocation);
  - a Wii replacement for the Xbox hard-disk cache layer.
- The scenario is not the game's tick. The game's `game_tick` and
  `game_frame` need a map.
- The engine's per-frame `game_frame` effects (particles, contrails, widgets,
  sound) mutate game-state pools by frame time. Their cadence behaviour needs
  a map to test.
- In-process engine re-initialization is unsupported upstream: the game-state
  allocation cursor survives `game_state_dispose`. A restart is a relaunch.
- Emulator only. Physical Wii is untested.
