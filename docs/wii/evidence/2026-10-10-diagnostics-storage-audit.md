# Diagnostics storage audit: AI debug records and profiling (HWI-015D)

HWI-015's first slice measured the linked Wii engine at 22,975,648 bytes of
MEM1, which left 294,912 bytes after GX. Two named storage groups looked like
diagnostics. This audit traces every writer and reader of both groups. Neither
group's storage feeds AI decisions, simulation state, game state, saves or
netcode. Both groups also hold small controls that do change the game when
they are set; those controls stay.

The Wii build now leaves out the diagnostic storage by default. The image
shrinks by 1,679,456 bytes, and MEM1 after GX grows from 294,912 to 1,974,272
bytes. The scenario digests are unchanged on PowerPC and on the i686 host.
[Numeric evidence and the full reader/writer inventory](2026-10-10-diagnostics-storage-audit.json)

**Result:**

- **Compiled out on Wii:** the AI's large debug records (545,252 bytes of
  BSS), its 30,359,424-byte debug heap (now one 25,980-byte shared record),
  and the profiler's 256-frame history (1,124,352 bytes of BSS).
- **Kept:** the controls of both groups. These are the AI cheats, the debug
  selection that player spawning reads, the AI-profile disable and random-move
  switches, and the print and render switches. Storage that only matched the
  group's name pattern is kept too (player and playlist profiles).
- **`ai_debug_initialize` runs again**, as upstream's does. It is no longer
  reported unsupported.
- **Other ports:** unchanged. All 15 affected units compile to byte-identical
  i686 objects.

## Build

| Item | Before | After |
|---|---|---|
| Source | clean `0c73eaf8` | clean `fa192731` |
| Build ID | `ffbd172161ac9401` | `02ae7bd4f03ae987` (`engine_diagnostic_storage: omit`) |
| `engine.dol` | 4,101,856 bytes, `d55704a2…1d5c` | 4,091,968 bytes, `37686b4a…ddc4` |
| Toolchain | devkitPPC GCC 16.1.0, binutils 2.46.0 | same |

The option is `configure.py --wii --wii-diagnostic-storage omit|keep`, and
`omit` is the default:

- **`omit`** compiles the engine units with `-DHALO_AI_DEBUG_RECORDS=0
  -DHALO_PROFILE_FRAME_HISTORY=0` and does not wrap `ai_debug_initialize`.
- **`keep`** compiles both groups as HWI-015's first slice did and reports
  `ai_debug_initialize` unsupported. Its build, `950374d87b522b6a`, has
  exactly the "before" group sizes. Its image is 480 bytes larger than the
  "before" image only because of the new driver report.

Both defines default to 1 in the sources. The Linux, Windows and Android
build scripts do not mention them, and a unit test checks both facts.

## Verdict

| Group | Storage | Verdict | Action |
|---|---|---|---|
| AI debug | per-actor records `actor_debug_array` (heap 26,603,520) | diagnostic-only | one shared record (`ACTOR_DEBUG_INFO`) |
| AI debug | path debug storage `actor_path_debug_array` (heap 3,755,904) | diagnostic-only | none (`NULL`) |
| AI debug | `ai_debug` records: line-of-sight points/rays, debug path state/storage, firing-position evaluation (545,252 of 547,628) | diagnostic-only | compiled out |
| AI debug | `ai_debug` small records (line of fire, ballistic arc, idle look, speech, path end points; about 1.9 KB) | diagnostic-only | kept (small) |
| AI debug | `ai_debug` controls | **gameplay-affecting when set** | kept |
| Profiling | `profile_globals.frames[256]` (1,124,352) | diagnostic-only | compiled out |
| Profiling | the rest of `profile_globals` (5,456), section profiles, controls, rasterizer profiler (1,120) | diagnostic-only | kept |
| Profiling | `ai_profile` (3,820) | **mixed**: `disabled` and `move_actors_randomly` are gameplay-affecting when set; the meters are diagnostic-only | kept |
| "Profiling" by name only | `player_profile_globals`, `playlist_profile_*`, `profile_list` (1,928) | not profiling: player/playlist profiles and their menu | kept |

### Dependencies on the kept controls (exact)

These controls stay because they change gameplay when they are set. They
default to off or none, and only the console, debug keys or scripts change
them through `hs_globals_external.c`.

- **`ai_debug.selected_squad_index`** decides where players spawn
  (`source/game/players.c:1062`, `:1093`, `:1288`, `:1417`). When an
  encounter is selected, `player_get_starting_location_count` and its
  neighbours use that encounter's player starting locations.
  `ai_debug_initialize` sets the selection to none (`ai_debug.c:265`).
  - HWI-015's first slice wrapped `ai_debug_initialize`, so the selection
    stayed at 0 and not none. With a map loaded, an encounter could then have
    been selected.
  - The driver now checks for none after start-up (`STAGE
    diagnostic_storage`).
- **AI cheats** (each takes effect only in a local game):
  - `blind`, `deaf`, `ignore_player`, `invisible_player`:
    `actor_perception.c:3836`, `:4206`, `:4207`, `:4269`; `ai.c:929`,
    `:1450`; `actors.c:3489`.
  - `flee_always`: `actions.c:1148`.
  - `force_crouch`, `oversteer_disable`: `actor_moving.c:3439`, `:2725`.
  - `evaluate_all_positions`: `actor_firing_position.c:2389`.
  - `fast_los`: `ai.c:2324`.
  - `force_all_active`: `encounters.c:4085`, `:4111`.
  - `path_disable_smoothing`, `path_disable_obstacle_avoidance`:
    `path.c:939`, `:959`.
  - `force_vocalizations`, `disable_wounded_sounds`: `unit_dialogue.c:288`,
    `:300`, `:672`, `:856`.
  - The `communication_*` switches: `ai_communication.c`.
  - The editor-only tag fixes `fix_*`: `ai_debug.c:4738`, `:4776`.
- **`ai_profile.disabled`** (HS `ai_profile_disable`) skips the AI update,
  and **`ai_profile.move_actors_randomly`** (HS `ai_profile_random`) moves
  actors at random (`ai.c:1940`, `:1941`).

Line numbers are at `0c73eaf8`. The JSON lists every reference, grouped by
member.

## AI debug records

### Symbols (BSS, linker map)

| Symbol | Object | Before | After |
|---|---|---:|---:|
| `ai_debug` | `ai/ai_debug.o` (COMMON) | 547,628 | 2,376 |
| `global_ai_debug_drawstack_last_position`, `_next_position` | COMMON | 24 | 24 |
| `global_ai_debug_activation_cluster_bit_vector` | | 64 | 64 |
| `actor_debug_array`, `actor_path_debug_array`, `global_ai_debug_drawstack_height` | | 12 | 12 |
| `global_ai_debug_path_render_id` | | 2 | 0 (unreferenced) |
| **Total** | | **547,730** | **2,476** |

The members compiled out of `ai_debug` are listed below. The control
switches (offsets `0x0`..`0xF8`) are unaffected; their offset assertions
still hold.

| Members | Bytes |
|---|---:|
| `lineofsight_overflowed` .. `lineofsight_pair` (16,384 points, 8,192 rays) | 311,308 |
| `path_state`, `field_608A8`, `path_storage` (the `ai_debug_path` tool) | 199,524 |
| `evaluation_context_valid`, `evaluation_context`, `actor_record[512]` | 34,420 |

Heap use:

- **Before:** `ai_debug_initialize` would allocate 1,024 × 25,980 bytes plus
  32 × 117,372 bytes (30,359,424 in total). The first slice skipped it.
- **After:** it allocates one 25,980-byte record.

Code in the group shrinks from 50,620 to 41,992 bytes.

### Writers and readers

Of 1,618 references in the inventory, 0 are unclassified.

- **Per-actor records: written by the AI on every decision.**
  - The 16 sites that take an actor's record now use `ACTOR_DEBUG_INFO`:
    - `actions.c:1784`, `:2074`, `:3504`, `:4445`;
    - `action_charge.c:531`, `:909`;
    - `action_vehicle.c:457`;
    - `actors.c:1727`, `:2372`;
    - `actor_combat.c:1105`, `:1643`;
    - `actor_moving.c:1422`, `:2012`;
    - `actor_perception.c:2513`, `:2695`, `:6521`.
  - Through those pointers there are 212 field writes and 61 address-taking
    output arguments (for example `point_from_line3d(…, &debug_info->field_108)`).
- **Per-actor records: read** in only three places outside the debug drawing:
  - `actors.c:1749` and `:1752` count a firing-position type mismatch for
    150 ticks, then log a silent `error` to `debug.txt`.
  - `actor_perception.c:6585` dedupes the awareness-speed print, which
    `print_acknowledgement` gates. The value only decides whether the record
    is rewritten and printed.
  - Every other read is `ai_debug_render_actor` (`ai_debug.c:767`..`:3774`),
    which only `render_debug.c:514` reaches.
  - With one shared record, these readers see the last actor's values. Only
    debug logging and drawing change.
- **Path debug storage.** `ai_debug_get_path_storage`
  (`actor_firing_position.c:2272`, `actor_moving.c:2068`) passes a slot to
  `path_state_new`.
  - **`path.c`:** the reads at `:853`..`:1858` only choose debug writes and
    debug assertions.
  - **`path_obstacle_avoidance.c:905`..`:965`:** the obstacle search keeps
    its working obstacles in the slot instead of in locals.
    - It would reuse them only when `use_stored_obstacles` is set. No code
      writes it, and `ai_debug_get_path_storage` zeroes the slot, so the
      obstacles are always recomputed.
    - `path_new` initializes every field the search reads, so the storage
      location does not change the result.
  - Without the storage, `ai_debug_get_path_storage` returns `NULL`.
    Upstream already passes `NULL` at `action_flee.c:240` and
    `actor_firing_position.c:1692` and `:2222`.
- **`ai_debug` records.**
  - Line of sight is written only when `render_lineofsight` is set
    (`ai.c:2303`).
  - The debug path is written only when `ai_debug.path` is set
    (`ai_debug.c:4633`..`:4736`).
  - The firing-position evaluation is written only for the debug-selected
    encounter (`actor_firing_position.c:2056`..`:2431`).
  - All three are read only by `ai_debug_render_*`.
- **Not in the game state, saves or netcode.** No reference is in
  `source/networking` or the saved-game code. The only saved-game hook is
  `game_state.c:201`, which calls `ai_debug_initialize_for_new_map` after a
  load to clear the records and re-select by name.

## Profiling storage

### Symbols (BSS, linker map)

| Symbol | Object | Before | After | Class |
|---|---|---:|---:|---|
| `profile_globals` | `cseries/profile.o` | 1,129,808 | 5,456 | diagnostic-only (frame history removed) |
| `ai_profile` | `ai/ai_profile.o` (COMMON) | 3,820 | 3,820 | mixed: two gameplay switches; kept |
| `profilestring` | `ai/ai_profile.o` (COMMON) | 2,048 | 2,048 | diagnostic-only |
| `profile_list` | `linux/game/menu_functions.o` | 1,676 | 1,676 | not profiling (profile menu) |
| `rasterizer_profile_*`, `local_profile_enable` (8) | `rasterizer/xbox/rasterizer_xbox_profile.o` | 1,120 | 1,120 | diagnostic-only (Xbox rasterizer) |
| `playlist_profile_globals`, `playlist_profile_write_options` | `saved_games/playlist_profile.o` | 144 | 144 | not profiling (saved playlist) |
| `player_profile_globals` | `saved_games/player_profile.o` | 108 | 108 | not profiling (player profile) |
| `profile_accumulated_*`, `profile_accumulation_index` | `rasterizer/rasterizer_frame_statistics.o` | 10 | 10 | diagnostic-only |
| `profile_global_enable`, `profile_timebase_ticks`, `profile_dump_frames`, `profile_dump_lost_frames`, `profile_graph`, `profile_display`, `global_ai_profile_string_position` | | 8 | 8 | diagnostic-only (switches) |
| **Total** | | **1,138,742** | **14,390** | |

Code in the group shrinks from 28,292 to 27,824 bytes. Data is unchanged at
34,079 bytes, mostly the profile graph's definitions
(`profile_graph_values`, 33,536).

### Writers and readers

- **Writers.**
  - The frame and tick timers: `main.c:2338`, `:2534`, `:2537`, `:2541`,
    `:2581`, `:2616`, `:2631`, `:2663`, `:3306`, `:3405`..`:3428`;
    `game.c:312`, `:376`; `render.c:194`..`:434`; `object_lights.c:538`,
    `:543`; `rasterizer_xbox.c:1950`, `:1952`;
    `rasterizer_xbox_profile.c:582`.
  - The 40 `profile_enter` and `profile_exit` pairs. These run only while
    `profile_global_enable` is set, and they write each section's own
    counters.
  - The frame history is written only by `profile_frame_end`
    (`profile.c:1500`).
- **Readers.** Every function that returns profile data is read only by the
  debug overlays and dumps:
  - `profile_frame_iterator_*`, `profile_frame_get_value`,
    `profile_frame_get_stalls`, `profile_frame_get_messages`,
    `profile_find_frame_value` and `profile_dump`: `interface.c:914`, `:963`,
    `:1009`..`:1037`, `:1134`..`:1171` (`profile_display`,
    `profile_graph`).
  - The HS commands `profile_dump`, `profile_activate`,
    `profile_deactivate` and `profile_graph_toggle` (`hs.c:14838`..`:14841`).
  - The frame dump writes `d:\framedump.txt`.
  - The frame history itself is read at `profile.c:636`, `:751`, `:1011`
    and `:1527`.
  - No return value reaches the game loop, the tick, the game state, a save
    or a network message.
- **Without the history**, the graph iterator yields no frame, so the graph
  draws nothing. The frame dump writes each frame as it ends. The section
  profile (`profile_dump`, `profile_display`) is unchanged.

## Nothing changed: scenario, host and layout

| Check | Result |
|---|---|
| PowerPC cadence scenario, launch 1 | 18/18 runs identical; digest `c1ad6ae6`, chain `58ce9719`; checkpoints `ef3efead`/`1370c5e7`/`fb1f5683`; seed `56096e69` |
| PowerPC cadence scenario, launch 2 | same; previous launch's persisted digest equal |
| i686 host reference (`tools/wii/run_engine_scenario_host.py`, Debian clang 19.1.7) | every cadence `c1ad6ae6`/`58ce9719`, the same frame counts as before; `END result=pass` |
| Game-state layout (`engine_layout.py game-state`, with the new engine flags) | 75/75 rows; 17,804,952 bytes on both; no difference |
| Upstream objects | 15 units compile to byte-identical i686 objects (Linux game flags, no `-g`) at `0c73eaf8` and `fa192731`: the 10 changed units and 5 that read `ai_debug` |
| Python tests | 322 tests OK on Python 3.14 and on 3.11 |

The host scenario does not compile the AI or the profiler. Its equality shows
that the scenario units are untouched. The PowerPC launches run the whole
engine with the storage removed.

## MEM1

| | Before | After |
|---|---:|---:|
| Image `0x80003f00..` | `..0x815ed3a0`: **22,975,648** | `..0x81453340`: **21,296,192** |
| BSS | 18,872,810 | 17,203,204 |
| Code | 3,212,604 | 3,203,248 |
| Read-only data | 701,584 | 701,104 |
| Data | 186,806 | 186,806 |
| Left by the image | 2,174,048 | 3,853,504 |
| Arena at `main` | 2,108,512 | 3,787,968 |
| **After the GX FIFO and two framebuffers** | **294,912** | **1,974,272** |
| After engine start-up | 16,384 MEM1, then the heap in MEM2 (802,816 during the cycles) | 892,928 MEM1; MEM2 heap unused (`arena2_lo` stays `0x90002000`) |

The "before" runtime figures come from HWI-015's first slice. Its image was
the same 22,975,648 bytes. The "after" figures are this build's log (`MAP`
lines).

## Two cold launches (stock Dolphin `1ed5e3f6…0dac`, Wii mode)

| Check | Launch 1 | Launch 2 |
|---|---|---|
| FPSCR at entry → engine | `0x4` → `0x0` | `0x4` → `0x0` |
| Platform checks | 8/8 pass | 8/8 pass |
| `game_initialize` | completes; 17,986,420 bytes of game state; 6 unsupported names (was 7) | same |
| `STAGE diagnostic_storage` | records 0, history 0, `ai_debug` 2,376 bytes, one shared record, no path storage, initialized, selection −1/−1, pass | same |
| Cadence runs (3 cycles × 6) | 18/18 identical | 18/18 identical |
| Heap across cycles | flat | flat |
| MEM2 arena bound after shut-down | restored (`0x933db7e0`) | restored |
| Previous launch's digest (persisted) | none (first) | equal |
| Launch counter read back | 1 | 2 |
| Engine warning in `d:\debug.txt` | present | present (both launches') |
| Halt path (deliberate fatal assertion) | reported, exit | reported, exit |
| Guest (log `END`) | pass, 0 failures | pass, 0 failures |
| Persistence (runner) | passed | passed |
| Host exit | `0x0`, natural, 35.96 s | `0x0`, natural, 34.70 s |

- **Unsupported calls:** 8 names and 15,508 calls, against the first slice's
  9 names and 15,509 calls. `ai_debug_initialize` is no longer reported.
- **OS:** in the batch window (17:10:43–17:12:06 UTC) the runner's event-log
  query found no instability event and no Application event naming Dolphin.
- **Runner's guest verdict:** `not_evaluated`, because the runner has no
  engine scenario checker. The guest result in the table is the log's own
  `END` line plus the comparisons above.

## Timings (Windows 11 Pro build host, Core i7-12700, 32 GB)

| Step | Wall time |
|---|---:|
| `ninja wii_engine`, full, before (`0c73eaf8`) | 80.8 s |
| `ninja wii_engine`, full, `keep` (`fa192731`) | 80.0 s |
| `ninja wii_engine`, full, `omit` (`fa192731`) | 68.7 s |
| i686 host reference (WSL Debian, including WSL start) | 49.7 s |
| Game-state layout census | 39.1 s |
| Dolphin launches | 35.96 s, 34.70 s |
| Dolphin batch, including the lock | 84.6 s |

The Research Mac was not used.

## Limits

- **No AI ran:** no map loads yet. The audit is of the source, every
  reference at `0c73eaf8`. The runtime evidence shows start-up, the debug
  state and unchanged scenario digests, not an AI decision.
- **Debug output changes.** With the records left out, the debug drawing
  that shows per-actor records reads the shared record. The firing-position
  type-mismatch warning and the acknowledgement print dedupe are not per
  actor. The `ai_debug_path` tool and the profile graph do nothing.
- **Emulator only.** Physical Wii is untested (HWI-040).
