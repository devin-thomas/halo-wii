# A campaign map with scripts and AI, tick by tick, and recoverable load failures (HWI-016C)

The Wii engine build now loads the first campaign map, a10, with all nine of
its structure BSPs. It runs the engine's own tick on it for 9,000 ticks
(five minutes of game time at 30 ticks a second): the opening cinematic, its
four script-driven BSP switches, the AI and the map's 105 script threads. A
labelled script command switches the BSP too.

In stock Wii-mode Dolphin every tick's state is identical to the i686 Linux
host's running the same map and plan: the whole game state and the
simulation, after every one of the 9,000 ticks, at 30 Hz, 60 Hz and with
frames paced by the measured vertical retrace. This holds over two cold
launches on each of two emulator hosts. Prisoner (HWI-015B) keeps its
simulation digest `127656ef`.

A staged map that is missing, truncated, damaged, stale, listed with another
staging in the manifest, or looked for under a wrong data root is refused
with an error that says what to copy again. The engine does not halt, and the
next map loads. A damaged BSP is refused when a switch reaches it, and the
BSP before stays loaded. [Numeric evidence](2026-10-10-campaign-map-tick.json)

**Result:**

- **Staged:** a10's 3,357 tags (14,158,000 bytes) and its 9 BSPs (largest
  8,568,832 bytes), 53,457 relocated pointer fields, byte-identical on Titan
  and on the Research Mac. Every byte left unconverted is classified.
- **Run:** 9,000 ticks with the scenario's scripts, AI and a scripted player:
  the cinematic, BSP switches 0 → 7 → 1 → 8 → 0 made by the scripts, the
  cinematic's end at tick 8,100 (logged every 150 ticks; between 7,950 and
  8,100) and the AI's 2 actors becoming 4. A `switch_bsp` script command, run
  after tick 300 of a labelled run, switches to BSP 2.
- **Host comparison:** in all four launches (Titan 1 and 2, Research 1 and 2)
  all 24 runs and both warm-ups equal the host's: raw and canonical
  whole-state and simulation digests and their chains over every tick, 1,794
  allocation checkpoint digests, and the refused loads. First divergent
  tick: none.
- **Script runtime on big-endian:** 45 reads and writes of boolean and short
  script value cells were casts of the cell's long, which take the wrong end
  of it on PowerPC; on the Wii, a10's mission script never woke from its first
  `sleep_until`. The engine now reads and writes each cell in place. A source
  audit test checks every read, and a control launch with the old runtime
  shows the failure (below).
- **Load failures:** all six kinds refused with the expected kind in every
  launch, then Prisoner loads and still gives `127656ef`.
- **Divergences found and resolved:** 15 (below); none hidden.
- **Memory:** a10 fits: the tag slot holds 22,726,832 of its 23,068,672 bytes
  at a10's largest BSP. With drawing off, MEM1 has 589,824 bytes free after
  start-up and MEM2 2,985,984 spare, and the heap stays in MEM1. With drawing
  on, a10's first BSP's geometry load needs 8,100,736 bytes against 3,575,808
  free: short 4,524,928 bytes, escalated (Memory).

## Build

| Item | Value |
|---|---|
| Source | Clean `bedbaaf3` on `wii` at `e8bc0e6d` (HWI-016B merged) |
| Build ID | `a0fb01de7eb68432` (diagnostic storage `omit`) |
| Toolchain | devkitPPC GCC 16.1.0, binutils 2.46.0 |
| `engine.dol` | 4,111,584 bytes, SHA-256 `3dcb71b3278f30117dd922ca0e03044de8e50bd0bf9a846ec17e7fde2a0036ef` |
| Host reference | Debian clang 19.1.7, i686, the same commit |
| Render | `render.txt` `render=off` on the Wii; the host draws nothing. The observer camera is omitted on both sides, and `rasterizer_initialize` stays unmade on both (HWI-016B's rule). One more launch measures a10 with drawing on (Memory). |

The CI's build-only package passes (`package_artifacts.py stage` and
`inspect`, as the workflow runs them): 33 files, 0 problems, 0 findings.
`engine_unsupported.py --check` passes. Tests pass on Python 3.14 and 3.11:
457 tests.

## Staging a campaign map

`tools/wii/map_stage.py` now stages every structure BSP of a map, in format
`HWIMAP02`. The file holds a 64-byte table, the map's header, then segments:
the tag data and one per BSP. Each segment records its kind, its source
offset, its bytes, where it goes in the tag slot, its relocations and a CRC
of its bytes and relocations. The table's own CRC is the staging digest.

The engine loads every BSP of a map at the same place, the top of the
22 MiB tag slot, so each BSP is walked and relocated for its own slot
offset. The tag data must walk to the same records under every BSP; the
stager refuses the map otherwise.

| a10 | Value |
|---|---:|
| Tags | 3,357 (14,158,000 bytes) |
| Structure BSPs | 9, from 1,529,856 to 8,568,832 bytes |
| Largest BSP's slot offset | 14,499,840 (341,840 bytes free between the tags and it) |
| Elements converted by their schema layouts | 2,986,997 |
| Tag data fields converted by explicit rules | 8,927 |
| Model part triangle indices | 215,054 |
| Pointer fields relocated; nulled | 53,457; 84,589 |
| Staged file, each byte order | 69,535,208 bytes |

**New explicit decoders, each with tests on authored fixtures**
(`test_map_stage.py`):

- **Script constants by their constant type.** A syntax node's value is a
  32-bit cell. A boolean constant is its first byte, a short-cell constant
  (the engine's short, enum and name types) its first two bytes, anything
  else a long. The type is the node's constant type (its index), not its
  node type: an object constant is its object name's short cell, which the
  map's load writes again in place.
- **Recorded animations.** The engine reads a recorded animation's event
  stream natively: its unit control data, its animation state and each
  event's payload. The stager decodes each of a10's 99 streams by the
  stream's own versions and converts each multi-byte field. A version the
  engine does not play is refused.
- **BSP data:** 32-bit words and bytes in all 9 BSPs.

**Unconverted bytes, all classified.** 4,323,188 loaded bytes are left as
they are, each attributed:

| Kind | Bytes |
|---|---:|
| Vertex and index payloads only the renderer reads | 4,100,446 |
| Tag names (8-bit text) | 164,216 |
| Targets the walker does not reach (the engine does not read them) | 22,963 |
| Alignment between records | 13,627 |
| Two HUD meter tags with no schema definition | 12,408 |
| Zero bytes | 9,524 |
| Other | 4 (one run) |

The stager checks that each loaded range holds the same multiset of bytes
in both orders.

**Determinism.** Staged by the final commit's tools on both hosts, the
outputs are byte-identical:

| Output | SHA-256 | Titan | Research |
|---|---|---|---|
| a10, big-endian (staging digest `7dc74a74`) | `810be90c…` | same | same |
| a10, little-endian (`4ecdcfb5`) | `d0f9ec7c…` | same | same |
| Prisoner, big-endian (`3189bf32`) | `fecad67a…` | same | same |
| Prisoner, little-endian (`af3ced28`) | `d130acf0…` | same | same |
| `manifest.txt`, `manifest.le.txt` | `76bec653…`, `ab153a98…` | same | same |
| Load-failure files (10) and their two manifests | in the JSON | same | same |

Staging a10 takes 13.1 s on Research and 135.6 s on Titan, where the walker
is built in WSL and the run shared the machine with a Dolphin launch
(Timings).

## Loading, and refusing what cannot be used

`port/wii/engine/cache_files_wii.c` checks a staging before it uses any of
it:

1. the data root's `maps/manifest.txt` exists and lists the map;
2. the file's table: its magic, byte order and format version are this
   build's;
3. its size, staging digest and header CRC match the manifest's line;
4. each segment's CRC, read into the tag slot, before any relocation is
   applied.

A failure is refused with a kind and a sentence in the log
(`MAP_ERROR kind=... map=... text="..."`), also given to the engine as a
silent error. The engine's load fails as a missing map fails: nothing is
left half-loaded, and the next load works. The sentence says what to do, for
example to copy the file again from `map_stage.py`'s output, or to stage the
map again with this build's `map_stage.py`.

`tools/wii/map_load_failures.py` makes the failing files from the owner's
stagings; they are private.

| Plan line | What is on the card | Refused as |
|---|---|---|
| `nomap` | no staging, no manifest line | `not_staged` |
| a10, from `d:\wrong_root\maps\` | no `manifest.txt` there | `data_root` |
| `prisonertrunc` | the first half of the staging | `damaged` (its size) |
| `prisonerflip` | one byte of its tag data changed | `damaged` (the tag data's CRC) |
| `prisonerold` | a format-1 staging (HWI-015B's) | `stale` |
| `prisonermm` | the manifest lists a10's staging digest for it | `manifest_mismatch` |
| `a10bsp` | one byte of its second BSP changed; switched to after tick 5 | `damaged`, at the switch |

A damaged BSP is found only when it is asked for. The engine's BSP switch
used to treat a BSP that failed to load as fatal and exit. It now reports a
silent error and loads the BSP it had (a small commented engine change in
`scenario.c`). The switch's command runs, the BSP index is the same before
and after the switch, and the run goes on.

In all four launches every refused load was refused with the expected kind
(6/6), each in 1–156 ms of guest time (the changed-byte file is read whole
before its CRC fails). The damaged BSP was refused at the switch with the BSP
before kept (7 before and after). Prisoner then loaded and gave `127656ef`
again, and nothing halted (`END result=pass`, 0 failures). The host refuses
the same loads with the same kinds.

## The run

Each map line of the plan (`map.txt`, format in
`port/wii/engine/engine_map_run.h`) loads its map through the engine's own
`game_load` and `game_initialize_for_new_map`. It makes a local player, runs
the engine's own scheduler for the line's ticks at one render cadence with
input scripted per tick (HWI-015B's script), and unloads. The evidence plan,
in order:

| Line | Runs |
|---|---|
| Prisoner, as HWI-015B | 18: three cycles of six cadences, `expect=127656ef` |
| The six refused loads | — |
| Prisoner again, 30 Hz | 1, `expect=127656ef` |
| `a10bsp`, 10 ticks, `switch_bsp=5:1` into the damaged BSP | 1 |
| `a10sw` (a10 under another name), 600 ticks, `switch_bsp=300:2` | 1 |
| a10, 9,000 ticks at 30 Hz, 60 Hz and retrace-paced | 3, after a 30-tick warm-up |

What a campaign map needed, beyond Prisoner:

- **The main loop's game requests.** Scripts ask for BSP switches,
  checkpoint saves and reverts through flags that the engine's main loop
  carries out between frames. The run now calls `main_port_game_requests`
  (new in `main.c`) around each update, as the main loop does. Without it,
  the scripts wait for ever.
- **History at load.** A map load leaves counters that the next load reads:
  data arrays' next identifiers, the local random seed and the dialogue
  variant counter (now external). Each load starts them where the first load
  had them.
- **Script threads in the digest.** A thread's stack packs frames at 2-byte
  offsets. Each thread is digested frame by frame, over its live frames.
- **The decal vertex cache** is not made without a rasterizer:
  `rasterizer_decal_vertices_new` answers none, reported unsupported.

### What the a10 run covers

The 30 Hz run, as logged every 150 ticks (the same on the Wii and the host,
in every launch):

| Tick | BSP | Cinematic | Actors | Objects | Script threads |
|---:|---:|---|---:|---:|---:|
| 1 | 0 | no | 2 | 358 | 105 |
| 150 | 7 | yes | 2 | 351 | 105 |
| 1,350 | 1 | yes | 2 | 373 | 105 |
| 3,750 | 8 | yes | 2 | 362 | 105 |
| 7,050 | 0 | yes | 2 | 362 | 105 |
| 8,100 | 0 | no | 4 | 405 | 105 |
| 9,000 | 0 | no | 4 | 404 | 105 |

The 105 script threads run throughout. The four BSP switches are the
scenario scripts' own, carried out through the main loop's requests.
Vehicles are staged and loaded as part of a10's tags, but the opening's
9,000 ticks drive none, so vehicle physics is not exercised.

The labelled command run (`a10sw`) has the engine's script runtime compile
and evaluate `(switch_bsp 2)` after tick 300, as a console command would.
The script switches to BSP 7 first, as in every a10 run; the command then
switches to BSP 2 (2 switches; the command's result 1, no request left), and
the run ends on BSP 2, equal to the host's at every tick.

## The i686 host reference, tick by tick

The host reference (`tools/wii/run_engine_map_host.py`) runs the same plan
on the little-endian stagings. It places MEM2 at the Wii's addresses,
including each launch's arena offset, and replays the launch's measured
retrace frame times. `tools/wii/compare_map_runs.py` compares the logs map
by map, run by run, allocation by allocation and tick by tick, and compares
the refused loads.

The Wii and the host place every region at the same address within a
launch, so the comparison now uses a **raw digest**: HWI-015B's byte
multiset of each 8-byte group, with pointers left as they are. The canonical
digest (pointers made region and offset, HWI-015B) stays at the checkpoints
and for the Prisoner regression across launches. a10 showed why: four bytes
that are not a pointer can read as one in one byte order only (measured in
a10's objects), so the canonical digest can differ where the state is equal.

| Launch compared | Runs, whole state equal | Runs, simulation equal | Allocation checkpoints equal | Ticks equal | Refused loads equal | First divergent tick |
|---|---:|---:|---:|---:|---|---|
| Titan 1 | 23/23 | 23/23 | 1,794 | 10,210 | yes | none |
| Titan 2 | 23/23 | 23/23 | 1,794 | 10,210 | yes | none |
| Research 1 | 23/23 | 23/23 | 1,794 | 10,210 | yes | none |
| Research 2 | 23/23 | 23/23 | 1,794 | 10,210 | yes | none |

The comparison keys runs by map, cycle and cadence: 23 keys, because the
second Prisoner line's 30 Hz run shares its key with the first line's. Every
`RUN` line was also compared whole: all 26 (24 runs and 2 warm-ups) are
equal in every launch, digests and chains, raw and canonical. The per-tick
lists cover each line's first run (a10 9,000 ticks, `a10sw` 600, `a10bsp` 10,
Prisoner 600); each run's chains cover every one of its ticks.

## Script runtime on big-endian

A script value is a 32-bit cell. A boolean is its first byte and a short its
first two bytes, as the Xbox's little-endian compiler laid them out.
`hs_runtime.c` read and wrote 45 such values as a cast of the long, for
example `(short)*value`. On a little-endian target that is the value; on
PowerPC it is the other end of the cell.

The fix (`hs.h`, `hs_runtime.c`, `hs_compile.c`) reads and writes each cell
in place (`HS_CELL_BOOLEAN`, `HS_CELL_SHORT`, `HS_CELL_SET_*`). The bytes
beside a value reach the game state's digest, so they are written as the
little-endian ports write them, equal on every port. The values on
little-endian ports are unchanged.

The proof has three parts:

- **A source audit** (`tools/wii/test_hs_cells.py`, 6 tests) reads the
  engine's script source. Every evaluator's argument views and result width
  must match its function's types; `hs_runtime.c` may not read a short or
  boolean cell by a cast; each `sleep_until` argument must be read as its
  parser types it. The pre-fix runtime fails the audit.
- **A control launch** with the pre-fix script runtime (`source/hs` from
  `e8bc0e6d`, all else `68d3b155`): 900 ticks of a10 on Research.
  The Wii never leaves BSP 0 and the cinematic never starts in
  900 ticks (0 switches); its state differs from the host's from tick 1. The
  same pre-fix runtime built for the i686 host switches to BSP 7 and starts
  the cinematic by tick 150, as the fixed runtime does on both. The old
  runtime works on little-endian and fails on big-endian; the fixed one is
  equal on both.
- **The evidence launches.** Every script thread's live frames and the
  script globals are in the whole and the simulation digests. All four
  launches equal the host at every one of the 9,000 ticks, and the
  script-driven events fall on the same ticks.

## Divergences found and how each was resolved

Each was found by the per-tick comparison and located by dumping the
allocation on both sides (`dump.txt` on the card). None is hidden: each is
fixed or classified explicitly.

| Found | Cause | Resolution |
|---|---|---|
| a10's scripts stalled on both sides: no BSP switch | The scripts' game requests (switch, checkpoint) are carried out by the main loop, which the run did not call | **Run:** `main_port_game_requests` (new, `main.c`) around each update |
| Wii only: the mission script never woke (BSP 0 for ever; the host switched by tick 150) | Script value cells read by casts of the long (above) | **Engine fix:** cell accessors in `hs.h`, `hs_runtime.c`, `hs_compile.c` |
| Stager: object constants staged wrong | A constant's cell is its constant type's (an object's is its name's short), not its node type's | **Stager:** by constant type, with tests |
| Wii crash at the first decal | No decal vertex cache without a rasterizer | **Unsupported, reported:** `rasterizer_decal_vertices_new` answers none |
| Runs differing within one process | Identifiers, the local random seed and the dialogue variant counter carried over from earlier loads | **Run:** each load starts them as the first load had them |
| The canonical digest differing where the bytes are equal | Four bytes read as a pointer in one byte order only | **Digest:** raw, every tick (above) |
| Script thread stacks differing | Unaligned frames and dead residue above the top frame | **Digest:** frame by frame over the live frames |
| A damaged BSP halted the game | `_error_immediate` on a BSP that failed to load | **Engine fix:** a silent error; the BSP before is kept (`scenario.c`) |
| Research only, from the first ticks: squads and platoons | 0/0 strength: x86 and the Xbox give the NaN `0xFFC00000`, PowerPC and ARM `0x7FC00000`. Titan's Dolphin (x86 JIT) gives the x86 NaN, so Titan matched by accident | **Engine fix:** the Xbox's bits on every port (`encounters.c`) |
| Tick 3634, script threads | A pushed frame's values held popped frames' bytes, partly written over, differently in each byte order | **Engine fix:** a frame's values zeroed when it is pushed (`hs_runtime.c`) |
| Tick 7963, actors | A secondary look direction was copied whole, with 2 bytes of stack padding | **Engine fix:** cleared, then its fields copied (`actor_looking.c`) |
| Tick 7987, actors | Union bytes that a prop's or object's direction leaves unset held stack bytes | **Engine fix:** only the type's union member is copied |
| Tick 150 of a10's second and third runs in a launch, object globals | A cluster activation wrote only the short half of a union; the other half held an earlier object index's bytes, a different half in each byte order | **Engine fix:** the union cleared first (`objects.c`) |
| Tick 8082, script threads | This ticket's own earlier change read `sleep_until`'s timeout, a long, as a short cell | **Engine fix:** read as a long; the audit ties each read to its parser's type |
| a10's simulation differs between cadences | Frame-coupled presentation on frames with several ticks or none | **Classified and measured:** reported per run (`invariance=0`), below |

**a10 between its runs.** Each a10 run equals the host's run at the same
cadence, tick by tick. The three runs differ from each other:

| Allocation | Differs at 60 Hz | Differs retrace-paced | From |
|---|---|---|---|
| objects | yes | yes | the end |
| object globals | yes | — | tick 1: state left by the run before (the 60 Hz run follows the 30-tick warm-up, the others a 9,000-tick run) |
| effect, effect location | yes | yes | tick 150 |
| particle, particle systems, their particles | yes | yes | tick 1 |
| object looping sounds | yes | yes | tick 30 |
| game sound globals | yes | yes | tick 1 |
| game time globals | — | yes | tick 1 (frame times) |

`game_frame` advances effects, particles and sounds by frame time, so a frame
without a tick (half of the 60 Hz frames) or with several moves them
differently. a10's cinematic makes effects that reach its objects. Prisoner,
with no such effects, stays invariant at all six cadences (its 18 runs give
`127656ef`). The objects' differing fields are not yet traced to the
effect that writes them; the run reports the difference (`invariance=0`)
rather than failing it.

## Memory

Measured in every evidence launch (`render=off`); the numbers are the same on
both hosts.

| Item | Bytes | HWI-016B |
|---|---:|---:|
| MEM1 image `0x80003f00..0x8149d1c0` | 21,598,912 | 21,349,792 |
| MEM1 free after GX | 1,671,168 | 1,921,024 |
| MEM1 free after engine start-up | **589,824** | 839,680 |
| MEM2 plan: game state 20 MiB, tag slot 22 MiB, sound 4 MiB, general 1 MiB, 2 MiB reserve | 51,380,224 | same |
| MEM2 spare above the plan | **2,985,984** | same |
| MEM2 heap growth with drawing off | 0 (the arena's low bound is unchanged at shut-down) | 0 |
| Game state allocated at start-up | 17,986,420 of 20,971,520 | same |
| Tag slot at a10's largest BSP: tags 14,158,000 + BSP 8,568,832 | **22,726,832 of 23,068,672** (341,840 free) | Prisoner 7,919,052 |
| Heap in use after each cycle (launch 1) | Prisoner 2,500,912; a10 2,501,008 (flat) | 2,500,912 |
| General region peak; failed allocations | 65,536; 0 | same |

The image grew by 249,120 bytes over HWI-016B's, and MEM1 after start-up
shrank by 249,856. 205,600 bytes of that are the run harness's per-tick
arrays, now sized for 9,000 ticks: the host's reference digests, the run's
own digests (canonical and raw) and the retrace frame times. They are test
storage, not the game's. The rest is the loader's checks, the plan and the
script and AI fixes.

**a10 with drawing on** (`render=on`, one more Titan launch of the same
build: a10 for 9,000 ticks at 30 Hz, the environment staged from content
generation `17189aa32aa83526` with `tools/wii/render_stage.py`, private):

With drawing on, a10 cannot be drawn. Loading the first BSP's environment
geometry fails for memory (`RENDER_FAIL geometry_decode ... error=memory`), at
the map's load and again at the pose check, so nothing is drawn and the
launch's render check fails (`END result=fail`, 1 failure). The game itself
runs its 9,000 ticks; the arena is restored.

| Item | Bytes |
|---|---:|
| Free above the reserve after start-up: MEM1 589,824 + MEM2 spare 2,985,984 | 3,575,808 |
| First BSP's geometry load: HWL1 file 2,973,280 + its decoded form 5,127,456, held together | **8,100,736** |
| Largest BSP's geometry load: file 4,281,792 + decoded 7,382,560 | **11,664,352** |
| What the failed load owned at its peak (the file, then released) | 2,973,280 |
| MEM1 free at shut-down; MEM2 heap growth | 516,096; 4,096 |
| a10's environment for all 9 BSPs, as staged: geometry 26,654,976, lightmap pages 11,162,944, base maps 103,822,464 (RGBA8, generation `17189aa32aa83526`, before HWI-008F's CMPR profile) | 141,640,384 |

### Shortage, escalated (no capability cut)

| Need | Bytes |
|---|---:|
| Free above the reserve after start-up | 3,575,808 |
| First BSP: geometry load peak | 8,100,736 |
| **Short, first BSP's geometry alone** | **4,524,928** |
| **Short, largest BSP's geometry alone** | **8,088,544** |

These are before any lightmap page or base map is resident. Ways to close
it, for the coordinator: decode the HWL1 records in chunks straight into the
reduced GX arrays, so the file and the full decoded form never coexist (the
reduced form of the largest BSP is about 2.7 MB: 24 bytes a vertex and
6 a triangle); or stage the reduced GX-ready form at import so the Wii does
not decode; or lend the renderer the sound region (4 MiB, unused while sound
is not started) or the game state's unused 2,985,100 bytes. A build without
the run harness's per-tick arrays frees 205,600 bytes more of MEM1.

No shortage with drawing off. a10 fits in the tag slot with 341,840 bytes
to spare at its largest BSP, the heap stays in MEM1, and MEM2's 2,985,984
spare bytes are untouched.

## Stacks

- **Main stack:** peak 104,088 bytes below `main`'s frame, of 131,072
  (painted window 114,688, not exhausted). HWI-015B's Prisoner peak was
  97,920; a10's scripts, AI and BSP switches add the rest.
- **Engine threads:** every engine thread's stack is painted and read back
  when its routine returns (HWI-015B). The map run starts no engine thread;
  the one created, the platform check's, peaked at the 256-byte entry gap of
  its 65,536 bytes.

## Two cold launches on each host

Both hosts run stock Dolphin 2609 in a fresh isolated profile
(`tools/wii/run_dolphin.py`): Titan on Windows 11 with D3D, Research on
macOS 27 with Vulkan. Each pair is two cold launches of one profile, so the
second sees the first's persisted result and places the arena 4 KiB higher.
The runner records the guest as `not_evaluated`, because it has no scenario
checker for this DOL. The guest result below is the log's `END` line and
`MAP_PLAN`, plus the host comparison.

| Check | Titan 1 | Titan 2 | Research 1 | Research 2 |
|---|---|---|---|---|
| FPSCR at entry → engine | `00000004` → `00000000` | `00000004` → `00000000` | `00000004` → `00000000` | `00000004` → `00000000` |
| HWI-015A fixed-step scenario | `c1ad6ae6`/`58ce9719` pass=1 | `c1ad6ae6`/`58ce9719` pass=1 | `c1ad6ae6`/`58ce9719` pass=1 | `c1ad6ae6`/`58ce9719` pass=1 |
| Arena offset | 0 | 4096 | 0 | 4096 |
| Prisoner, 18 runs: simulation | `127656ef`, identical=1 | `127656ef`, identical=1 | `127656ef`, identical=1 | `127656ef`, identical=1 |
| Prisoner whole state (first run) | `6d88af44` | `6d88af44` | `6d88af44` | `6d88af44` |
| Expectations met | 9/9 | 9/9 | 9/9 | 9/9 |
| Refused loads (kind as expected) | 6/6 | 6/6 | 6/6 | 6/6 |
| Damaged BSP switch: BSP before, after | 7, 7 (pass=1) | 7, 7 (pass=1) | 7, 7 (pass=1) | 7, 7 (pass=1) |
| a10sw command: BSP at end, switches | 2, 2 (result 1) | 2, 2 (result 1) | 2, 2 (result 1) | 2, 2 (result 1) |
| a10 30 Hz: simulation / whole (raw) | `9e91d8f2` / `fae3ddf5` | `89ef148a` / `ad68a27e` | `9e91d8f2` / `fae3ddf5` | `89ef148a` / `ad68a27e` |
| a10 60 Hz: simulation / whole (raw) | `80bedd07` / `35f19a4c` | `8871d195` / `a93298c3` | `80bedd07` / `35f19a4c` | `8871d195` / `a93298c3` |
| a10 retrace-paced: simulation / whole (raw) | `7948ac8c` / `ecb7d9af` | `d6be3629` / `8a417184` | `7948ac8c` / `ecb7d9af` | `d6be3629` / `8a417184` |
| a10 BSP switches, objects at end | 4, 404 | 4, 404 | 4, 404 | 4, 404 |
| Heap across cycles | flat | flat | flat | flat |
| Arena after shut-down | restored | restored | restored | restored |
| Main stack peak | 104,088 | 104,088 | 104,088 | 104,088 |
| Persistence (runner) | passed | passed | passed | passed |
| Guest result (`END`) | pass, 0 failures | pass, 0 failures | pass, 0 failures | pass, 0 failures |
| Host exit, elapsed | `0x00000000`, 1114.401 s | `0x00000000`, 1114.016 s | `0x00000000`, 1110.869 s | `0x00000000`, 1110.871 s |

The halt path is still checked last: the deliberate assertion is reported
and the program exits.

Titan and Research give the same digests launch for launch. The second
launch's raw digests differ from the first's because its regions lie 4 KiB
higher; its host reference is placed the same way. Heaps are flat within each
launch (launch 2: Prisoner 2,500,944, a10 2,501,040). The Titan Dolphin uses
the x86 JIT and Research's the ARM64 JIT; the NaN they give for 0/0 differs
(the squads' fix above makes the result the same on both).

## Timings

| Step | Titan (Windows 11, WSL Debian) | Research (M5 Max, macOS 27) |
|---|---:|---:|
| a10 staging, both orders | 135.6 s, with the WSL walker's build, beside the render launch | 13.1 s (load 3.4) |
| Prisoner staging | 12.3 s | 1.4 s |
| Load-failure files | 1.0 s | 0.13 s |
| Wii engine build, incremental after this ticket's last code change | 59.3 s | not built there |
| Wii engine, all CI targets, incremental | 12.0 s | — |
| Host reference, the whole evidence plan (24 runs) | 47–57 s (per launch) | — |
| Dolphin launch, host elapsed | 1,114.4 s, 1,114.0 s | 1,110.9 s, 1,110.9 s (load 1.7 before, 2.3 after) |
| Guest, a10 load | 1,230 ms (cache reads 405 ms; Prisoner 400 ms) | same |
| Guest, a10's 9,000 ticks at 30 Hz with every digest | 303,876 ms (60 Hz 309,229, retrace-paced 317,644) | same |

The guest times are emulated time. Research's load average is given with
each Research time (`vm.loadavg`, 1 minute).

The control launch took 31.7 s on Research.

## Limits

- **Emulator only.** Physical Wii is untested.
- **Nothing is drawn** in the evidence launches (`render=off`). Drawing a10
  is measured once, for memory.
- **The opening only.** 9,000 ticks cover the opening cinematic, four BSP
  switches and the first AI. No vehicle is driven, and the player's input is
  HWI-015B's script, so combat is not exercised.
- **Cadence invariance** does not hold on a10 and is not claimed: its
  frame-coupled presentation differs between cadences. Each cadence equals
  the host's.
- **Unsupported, reported:** as HWI-015B, plus the decal vertex cache.
