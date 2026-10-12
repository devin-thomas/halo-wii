# A revert-class request stops a frame's ticks (HWI-034)

HWI-016F (now HWI-029) made a10's 30 Hz simulation independent of the
render cadence, but left one case open. Suppose a frame ran several ticks
and one of them asked for a revert, a loss, a core load or a won map. The
frame's remaining ticks still ran before the main loop handled the request
at the next frame. Those ticks ran on the state the request replaces. They
also drew the tick random numbers, which a revert does not restore.

The decision was option B: the frame stops running ticks at such a request.
The main loop handles the request at that tick boundary, and the next frame
runs the ticks that were left. This applies on every port. A frame of one
tick, as at 30 Hz on the Xbox, never stops, and no new code runs in it.

**Result:**

- **Host scenarios.** On a10, a script command asks for each request
  mid-frame:
  - a revert: `game_revert`;
  - a loss: `game_lost`, whose revert falls due 91 ticks later;
  - a core load: `core_load` of a core the harness saved;
  - a won map: `game_won`;
  - a map reset: `map_reset`.

  The scenarios ran on the i686 host at all six cadences: 30, 60, 20 and
  144 Hz, irregular pacing, and retrace pacing. For every request:
  - the simulation equals the 30 Hz run's at each of the 400 ticks;
  - no tick begins after the request in the frame it arrived in;
  - every tick the clock gave was run.

  Frames of several ticks stopped and carried the rest, up to 6 ticks in
  irregular pacing's 0.5 s hitch frame.
- **Before the change.** The same tree with the stop disabled diverges in
  the frame after the request, at 20 Hz and at irregular pacing. The first
  differing tick is 52 for the revert, 304 for the loss and 55 for the core
  load.
- **Wii.** In Dolphin, a won map and a map reset stop the frame as on the
  host, including in the retrace-paced run, where a measured retrace spans
  up to 7 ticks. Every run line equals the i686 host's: 42 of 42 in each of
  two cold launches on the Research Mac and two on Titan. Titan's and
  Research's lines are identical.
- **Blocked on the Wii: revert, loss, core load.** Loading a saved game on
  the Wii halts before any frame logic runs:
  `couldn't read saved game file (#8)`. The port reads the image, 20,971,520
  bytes, into a second buffer before taking it, and the Wii has no room for
  one. MEM2 has 2,985,984 bytes spare and MEM1 1,208,320 after GX. The
  first evidence plan halted there in all three launches it ran. These three
  requests are proven on the host only, and the save path needs a decision
  (below).
- **Regressions.**
  - a10's 9,000 ticks are still `f752db70` at every cadence.
  - Prisoner is still `127656ef` (HWI-015B's classification) and `961a5ec5`
    (HWI-016F's). Its whole state is still `7eb53576`.
  - HWI-015A's scenario is still `c1ad6ae6`.
  - HWI-016F's whole evidence plan, replayed on the host as its Titan
    launch 1, gives the same 3,164 report lines, digest for digest.
- **Dolphin.** Two cold launches each on Research (Vulkan) and Titan (D3D):
  - guest `END result=pass` in all four;
  - persistence passed (each second launch read `961a5ec5`);
  - one host shutdown fault on Research (`0xfffffff5`, launch 1, after the
    guest's `END`), and `0x0` everywhere else;
  - no OS instability events.
- **Physical Wii:** untested.

[Numeric evidence](2026-10-11-revert-in-multi-tick-frames.json)

## Build

| Item | Value |
|---|---|
| Source | Clean `d2a954cd` (on `wii` at `6d898c7c`), HWI-016G's build-ID rule |
| Build ID | `ea0efd5869fd46c4`, `source_dirty=false` |
| Toolchain | devkitPPC GCC 16.1.0, binutils 2.46.0 |
| `engine.dol` | 4,174,944 bytes, SHA-256 `f6e75c710470b405a15487b634a9f800a7e7e089a39ec4bcb3367926a41aeef2` |
| `engine.elf` | 37,797,304 bytes, SHA-256 `479a4ca576e292d2e813d057be856ec636011c7e1139d8bc59b03023c27bffe0` |
| Host reference | i686, Debian clang 19.1.7, the same engine units |
| Artifacts | `package_artifacts.py` stage and inspect `--require-clean`: 33 files, 0 problems, 0 findings |
| Maps | HWI-016C's stagings, unchanged (format HWIMAP02; the same hashes on both hosts) |

## The change (every port)

`game_time_update` (`source/game/game_time.c`) runs a frame's ticks in a
loop. After each tick that is not the frame's last, it asks
`main_port_revert_class_requests` (`source/main/main.c`) which requests are
waiting, one bit each:

| Bit | Request | Waiting when |
|---|---|---|
| 1 | revert | `main_globals.revert_map` |
| 2 | loss | `lost_map`, and its death timer would expire now (`main_lost_map_private`'s count, read without changing it) |
| 4 | core load | `load_core` |
| 8 | won map | `won_map` |
| 0x10 | map reset | `reset_map` |

A new bit ends the loop: one that was not already waiting as the frame's
ticks began. The ticks left go into `game_time_ticks_carried`, and the next
frame runs them before its own:

- **The update queue.** A local game's held-back ticks already have their
  updates queued. A revert or a core load empties the queue
  (`update_queues_reset_and_fill_with_lies`, among `game_state.c`'s
  after-load steps). Those updates are then built again from the next
  frame's input, as that frame's own are.
- **Not in the game state.** A revert does not take back the carried count.
  A new clock drops it: `game_time_start`, or
  `game_time_initialize_for_new_map` for a new map or a map reset.
- **Elapsed time.** `game_time_get_elapsed` reports the ticks a frame ran.
  Whatever advances by it once a frame, such as HUD timers or the
  checkpoint save's check, counts the held-back ticks in the frame that
  runs them.

The loss is the one request that waits while ticks run. `game_lost`, or the
last player's death, starts a 92-tick death timer, and the revert follows
when it expires. The loss bit is set only when the timer would expire, so
the frame stops at the tick after which the 30 Hz game reverts.

`main_port_game_requests_between_ticks` (HWI-016F) now lets that timer
count on between a frame's ticks, as `main_loop` would. It still bails
while a loss's revert is due, so a revert never happens between two ticks.
Before this change it bailed for all 92 ticks: a checkpoint save, a
script's BSP switch or a respawn during them fell on a frame boundary.

**At 30 Hz.** A frame of one tick never reaches the check and never holds a
tick back, so no new code runs. HWI-016F's evidence plan reproduces
HWI-016F's host log line for line (below).

**Not of this class:** a core save, a cinematic skip, a map change and the
menus' requests. A cinematic skip is the player's input, taken once a
frame. A core save that a script asks for mid-frame is still made at the
next frame (see "Not covered").

## The harness

- **Plan options.**
  - `script=<tick>:<source>` runs the source through the engine's own
    script compiler and runtime after that tick, as `switch_bsp=` does
    (`~` stands for a space).
  - `core_save=<tick>` has the harness save a core after that tick
    (`game_state_save_core`, as `main_save_core_private` does).
  - `name=` names a line's runs, so lines on one map compare apart.
- **RUN lines** report the revert-class requests seen (`requests`, as
  bits), the first tick after which one arrived (`request_tick`), and the
  ticks begun after it in the same frame (`ticks_after_request`). They also
  report the scheduler's counts (`game_time_port_tick_counts`):
  `clock_ticks`, `ticks_run`, `carried`, `frames_stopped`, `carried_total`,
  `most_carried`, and the update queue's resets.
- **Scripted input.** The run's scripted input is the input of the tick
  that runs each update. When the update queue resets, the next update
  built takes the next tick's input (`real_map_queues_reset`). At 30 Hz no
  update waits when the queue resets, so nothing changes there.
- **A won map or a reset** is taken by the driver, which stays on its map,
  so a later one is a new request (`main_port_game_requests_left_taken`).
- **Revert and core load on the Wii.** A saved game's load re-points the
  decal vertex cache's procedures
  (`rasterizer_decals_update_function_pointers`). The Wii build does not
  make that cache, so the first revert halted there. The step is now
  reported as unsupported, like the cache's other entry points (HWI-016C).
- **Comparison.** `compare_map_runs.py` fails a log in which a tick began
  after a request in its frame, or in which
  `clock_ticks != ticks_run + carried`. It compares the Wii's and the
  host's counts like the rest.

## Scenarios (i686 host)

The plan is four lines on a10, labelled by `name=`. Each runs 400 ticks
after a 30-tick warm-up, at all six cadences, 30 Hz first. Each request is a
script command run through the engine's own compiler and runtime after the
given tick.

The ticks were chosen so the request arrives mid-frame at 20 Hz and
irregular pacing. Tick 51 is the first tick of irregular pacing's 0.5 s
hitch frame, which runs 7 ticks. Frames at 60 Hz and 144 Hz never run two
ticks, so there the request always arrives at a frame's end.

| Line | Request | Arrives after tick | Simulation, every cadence | 20 Hz: ticks carried | Irregular: ticks carried |
|---|---|---:|---|---:|---:|
| `a10revert` | `game_revert`, to the checkpoint after tick 1 | 51 | `e825c57b`, every tick equal | 1 | 6 |
| `a10loss` | `game_lost` after tick 212; its revert due | 303 | `ade62b23`, every tick equal | 1 | 1 |
| `a10core` | `core_load` of a core the harness saved after tick 30 | 54 | `9453ab30`, every tick equal | 1 | 3 |
| `a10won` | `game_won` | 306 | `941dc7dd`, every tick equal | 1 | 1 |
| `a10reset` (separate plan) | `map_reset` | 51 | `941dc7dd`, every tick equal | 1 | 6 |

In each line, the 20 Hz and irregular frames that held the request
stopped.

Every run has `first_divergent_tick=-1` against its line's 30 Hz run and
`ticks_after_request=0`. In every run, `clock_ticks = ticks_run = 400`, and
nothing is left carried.

The revert, the loss and the core load each reset the update queue once,
and the held-back ticks' updates were built again. The frame after a stop
runs the carried ticks and its own: up to 9 at irregular pacing, against 7
before.

The won map and the reset are taken by a harness that stays on the map, so
their simulation is the same with or without the stop. They show that the
frame stops at the request.

### Before the change

This is the same tree with the stop condition made false. It is a
diagnostic build and is not committed.

| Line | 20 Hz | Irregular |
|---|---|---|
| `a10revert` | `74ed373e`, differs from tick 52; 1 tick ran after the request | `08676197`, differs from tick 52; 6 ticks ran after it |
| `a10loss` | `4ee3d51f`, from tick 304; 1 tick after | `4ee3d51f`, from tick 304; 1 tick after |
| `a10core` | `6dda478f`, from tick 55; 1 tick after | `dfc39533`, from tick 55; 3 ticks after |
| `a10won` | `941dc7dd`; 1 tick after | `941dc7dd`; 1 tick after |

The 30 Hz, 60 Hz, 144 Hz and retrace-paced host runs are unchanged.

An earlier version counted a pending loss's death timer between ticks but
did not bail once the loss was due. Under the diagnostic build it reverted
between two ticks, and the run halted when the next tick found the queue
reset. The final version bails while a loss's revert is due, as HWI-016F
bailed for any loss.

### What could not be reached

A player's death in a10's opening. `unit_kill` of the player's unit left
the player alive, with or without `object_can_take_damage` first, so no
loss followed during the opening cinematic. The loss was asked for with
`game_lost` instead. That is the request a death raises (`main_lost_map`,
`players.c`), with the same 92-tick death timer.

## Regressions

| Check | Host | Wii, all four launches |
|---|---|---|
| HWI-015A fixed-step scenario | `c1ad6ae6` / `58ce9719` | `c1ad6ae6` / `58ce9719` |
| Prisoner simulation, HWI-015B's classification (`expect=`) | `127656ef` | `127656ef` |
| Prisoner simulation, HWI-016F's (`expect_sim=`) | `961a5ec5` | `961a5ec5` |
| Prisoner whole state, 60 Hz after a 30 Hz warm-up | `7eb53576` | `7eb53576` |
| a10, 9,000 ticks at 30 Hz, 60 Hz and retrace | `f752db70` at arena offset 0, every tick equal | Launch 1 `f752db70`. Launch 2 `03181654` (arena offset 4,096), as in HWI-016F's launch 2 on both hosts and the host replay at that offset. Cadence-invariant in every launch. |
| a10, 9,000 ticks at all six cadences | `f752db70` at each, every tick equal | — |

HWI-016F's whole evidence plan was replayed on the host as its Titan
launch 1, with the same arena offset and retrace frame times. Compared with
HWI-016F's host log of that launch, all 3,164 report lines are identical:
RUN, ALLOC, TICKS, TICKS_RAW, EXPECT, FIXED_STEP, MAP_RUNS, LOAD_FAILURE
and BSP_FAILURE. The comparison leaves out timings and the new RUN fields.

No revert-class request arrives in these runs, so no frame stopped
(`requests=0`, `frames_stopped=0`) and no digest moved.

## Dolphin

The evidence plan (`map.txt`) has 14 lines: HWI-016F's 12, plus `a10won`
and `a10reset` at all six cadences. The revert, loss and core-load lines
are left out, because loading a saved game halts on the Wii (below). Each
launch's host reference runs the same plan at the launch's arena offset
(0, then 4,096) and replays the launch's measured retrace frame times.

| Launch | Guest | Plan | Run lines equal to the host | Revert-class check | Persistence | Host process |
|---|---|---|---|---|---|---|
| Research 1 | `END result=pass` | 12/12 expectations | 42/42 | pass | passed | **`0xfffffff5`** after the guest's `END`, 1,297.0 s |
| Research 2 | `END result=pass` | 12/12 | 42/42 | pass | passed (read `961a5ec5`) | `0x0`, 1,297.1 s |
| Titan 1 | `END result=pass` | 12/12 | 42/42 | pass | passed | `0x0`, 1,299.5 s |
| Titan 2 | `END result=pass` | 12/12 | 42/42 | pass | passed (read `961a5ec5`) | `0x0`, 1,299.7 s |

- **The scenarios on the Wii.** In every launch, `a10won` and `a10reset`
  give the same simulation at all six cadences (`941dc7dd`), equal to the
  30 Hz run at every tick.
  - Frames stopped at the request at 20 Hz and at irregular pacing.
  - The reset's retrace-paced run also stopped, in a frame of up to 7
    ticks. In the won map's retrace-paced run, the request arrived at a
    frame's last tick.
  - No tick began after a request in its frame, and every tick the clock
    gave was run.
- **Ticks compared with the host:** for each line's first run, a10 9,000,
  `a10sw` 600, `a10bsp` 10, Prisoner 900, `a10won` 400 and `a10reset` 400.
  None differ.
- **Between hosts:** all 42 run lines of each launch are identical on
  Titan and Research, the retrace-paced runs included.
- **Memory:** the heap is restored (2,126,040 and 2,126,072 bytes in use at
  the arena's close). The main stack's peak is 105,600 bytes.
- **OS stability:** no instability events on either host. Research's batch
  window holds one Dolphin crash report, matching launch 1's nonzero exit.
  The guest had finished, and launch 2 read the card back intact. This is
  the shutdown-only Dolphin fault class that ADR-017 defers to HWI-072. It
  is reported here, not tolerated away.

### The first evidence plan: halted at the revert

The first plan had 16 lines: HWI-016F's 12 and all four scenario lines.
Three launches ran: two on Research and Titan's first. Each completed
HWI-016F's lines, then halted at `a10revert`'s revert after tick 51. Titan's
second launch was stopped once the first showed the same halt, and the lock
was released.

The halt was `game_state_xbox.c:156: couldn't read saved game file (#8)`,
the Win32 code for not enough memory. The checkpoint save itself works on
the Wii: the file is written. Taking a saved game is what fails:

- `game_state_read_from_file`, and `game_state_read_core` for a core, read
  the whole image into a second buffer the size of the game state,
  20,971,520 bytes.
- They check it there (`game_state_image_accept`) before copying it in.
  That port change keeps a damaged file from replacing a good state.
- The Wii engine build has no 20 MB to spare. MEM2 has 2,985,984 bytes left
  after the arena plan, and MEM1 has 1,208,320 bytes after GX.

Research's first batch ran under a load average of 24 from other work; its
two launches took 1,133.2 s and 1,131.3 s.

## Timings

| Step | Host | Wall time |
|---|---|---|
| Host reference build, full | Titan (WSL Debian clang) | 70 s (147 s for the diagnostic build) |
| Host reference build, incremental | Titan | 16–22 s |
| Host run: the four scenario lines at six cadences | Titan | 23 s (26 s wall) |
| Host run: HWI-016F's evidence plan | Titan | 43 s |
| Host run: a10, 9,000 ticks at six cadences | Titan | 70 s |
| Host reference of one evidence launch | Titan | 59–69 s |
| Wii build: configure and `ninja wii_engine` | Titan | 140 s from scratch; 55 s for the clean evidence build |
| Two cold launches | Research, Dolphin Vulkan | 2,594 s (1,297.0 s and 1,297.1 s). The batch first waited 3,587 s for the lock. Load average 3.49 3.35 4.24 before, 3.17 2.37 2.35 after. |
| Two cold launches | Titan, Dolphin D3D | 2,631 s (1,299.5 s and 1,299.7 s) |
| First plan, two launches (halted) | Research | 2,281 s (1,133.2 s and 1,131.3 s); load average 24.18 before |

The guest's own elapsed time is 1,296.1 s in every launch.

## Not covered

- **Revert, loss and core load on the Wii.** Taking a saved game halts on
  the Wii (above). The stop itself is the same code on every port, and all
  three requests are proven on the i686 host. A won map and a map reset are
  proven on both.
- **A player's actual death in a10** was not reached; the loss was asked
  for with `game_lost`.
- **A core save that a script asks for mid-frame** is still made at the
  next frame. It is not a revert-class request. The harness saved its core
  directly, at a tick boundary. A console `core_save` arrives between
  frames and is unaffected.
- **Network games.**
  - A network co-op host's revert (`main_coop_host_revert`) is not
    exercised.
  - On a network server, held-back ticks whose updates a revert dropped are
    not rebuilt. Only a local game rebuilds them, and the co-op host's
    revert renumbers the clock itself (`network_coop.c`).
  - A distributed client's ticks follow its own clock and are carried.
- **The main loop's frame-coupled work** (camera, input, director) is
  unchanged, as in HWI-016F.
- **Physical Wii:** untested.

## Decision needed

Taking a saved game on the Wii needs room the Wii does not have. A revert
or core load reads the image into a second buffer of 20,971,520 bytes and
checks it there before taking it. Until this is resolved, these halt the
Wii:

- a revert;
- a loss, once its death timer expires;
- a checkpoint restart;
- a core load.

This is the save path (ADR-015's save image, HWI-046), not the frame loop,
so it is left for a decision:

- **(A) Check in pieces, then read in place.** Read the file in chunks and
  check each allocation against the live layout before anything is
  written, then read it into the game state. This keeps the guarantee that
  a damaged file never replaces a good state, and needs no large buffer.
  `game_state_image_accept` would have to be restructured to check a
  stream.
- **(B) Read in place, then check, on the Wii only.** If the check fails,
  reset the map, as a refused file already does. A revert discards the
  current state anyway, so the cost is this: a damaged file resets the map
  instead of keeping the state, and the reset runs on a state that failed
  its check.
- **(C) Use the canonical save image that ADR-015 records**
  (pointer-free, little-endian), loaded in pieces. This is the durable-save
  design and the most work now.

With any of these in place, the revert, loss and core-load lines rejoin the
Dolphin plan unchanged. They pass on the host today.

## Cross-port scope

`game_time.c`, `main.c`, `main.h` and `game.h` are shared by the Linux,
Android and Windows ports. Those ports draw frames at the display's rate, so
for them a frame of several ticks is a frame that ran late. Their frames
now stop at these requests too. A frame of one tick is unchanged. The
harness changes are Wii-only (`port/wii/engine`, `tools/wii`).
