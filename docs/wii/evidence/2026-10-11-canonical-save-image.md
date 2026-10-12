# Saved games through the canonical game-state image (HWI-076)

HWI-034 left three requests that halted the Wii build: a revert, a loss
(whose revert follows 92 ticks later) and a core load. All three load a
saved game. The port read the whole 20,971,520-byte image into a second
buffer to check it before taking it, and the Wii has no room for one: MEM2
has 2,985,984 bytes spare after the arena plan, MEM1 1,208,320 after GX.

Devin chose option C (ADR-021). Every port now saves and loads the game
through the canonical game-state image that ADR-015 accepted, written and
read in bounded chunks: a checkpoint, a revert to it, a loss's revert, a
core saved and loaded, and the saved game of "save and quit".

**Result:**

- **The original path's results, exactly.** On the i686 host, the original
  saved-game path (`wii` at `bae56603`) and the canonical image give the
  same report lines, every tick's raw digest of the whole state included:
  - HWI-034's revert, loss and core-load scenarios at all six cadences:
    1,592 of 1,592 lines;
  - HWI-034's evidence plan (Prisoner, the load failures, a10's BSP lines,
    a10 at 9,000 ticks, the won map and the reset): 4,258 of 4,258;
  - a10 at 9,000 ticks at all six cadences: 1,392 of 1,392.

  Every load gives back exactly the state its image was saved from (the CRC
  of the game state's whole room equals the save's), and writing the state
  again gives the same image (its SHA-256).
- **Damaged images are refused, the game state untouched.** Twelve damaged
  copies of a core are loaded through the core load's own path at each of
  six cadences, on the host and the Wii: a flipped payload byte, the same
  with its chunk's CRC made again, a flipped header byte, cut in half, one
  byte short, empty, another map, another build, another version, not
  complete, the other byte order, another layout. All are refused for their
  reason, and the game state's 20 MiB room is the same byte for byte. A
  core written by the Wii, loaded on the host, is refused, and the run is
  line for line the run with no core at all.
- **Pointers are rebound at any base.** A core saved at arena offset 0 and
  loaded at offset 4,096 writes back the same image (SHA-256), and the 400
  ticks after it equal the native offset-4,096 run's raw chain.
- **The Wii.** HWI-034's three blocked scenarios are back in the Dolphin
  plan, with an image test. Two cold launches each on Research and Titan
  from clean `8e3d0d84` all pass:
  - revert, loss and core load at all six cadences, each equal to the
    i686 host at every tick (63 of 63 run lines per launch);
  - every image event equal to the host's (227 of 227);
  - one shutdown-only fault on Research after the guest's `END`.

  See [Dolphin](#dolphin).
- **Memory.** No second buffer. The image code takes at most 71,684 bytes
  from the heap while saving and 66,188 while loading: one 65,536-byte
  chunk and its bookkeeping. A checkpoint on the card is 0.46 to 0.73 MB
  instead of 20 MiB.
- **Physical Wii:** untested.

[Numeric evidence](2026-10-11-canonical-save-image.json)

## Build

| Item | Value |
|---|---|
| Source | Clean `8e3d0d84` (on `wii` at `bae56603`), HWI-016G's build-ID rule |
| Build ID | `a496a5b302fcc6ed`, `source_dirty=false` |
| Toolchain | devkitPPC GCC 16.1.0, binutils 2.46.0 |
| `engine.dol` | 4,210,208 bytes, SHA-256 `130dce4da0e3c541091187e743ef786898ba09e264c2553cdf5f0aceea113db6` |
| Host reference | i686, Debian clang 19.1.7, the same engine units |
| Original-path reference | the same harness at `wii` `bae56603` |
| Maps | HWI-016C's stagings, unchanged (HWIMAP02; the same hashes on both hosts) |

The branch's last engine change (`59f70117`) comes after the Dolphin build.
It changes one path only: a core whose header names another map, build,
player count or byte order is refused instead of halting the game
(below). The Dolphin plan never loads such a core.

## The image (format version 2)

Specified in `source/saved games/game_state_image.c`. Every integer is
little-endian.

- **Header**, 512 bytes, with its own CRC. It holds the magic `HWGS`, the
  version, a completion flag written last, the byte order of the port that
  wrote it, the chunk size and count, and the payload's size and SHA-256.
  It also holds the game state's layout (its parts' sizes and a digest of
  every data array's, memory pool's and lruv cache's kind, offset, size,
  capacity and element size) and the upstream header's facts: map name,
  build number, player count, difficulty, allocation checksum and map
  checksum.
- **Chunks** of 65,536 payload bytes (the last fewer), each with a 16-byte
  header: `HWGC`, its index, its size and the CRC of its payload. The CRC
  is the engine's (`crc.c`).
- **Payload**: the game state's words, little-endian, the CPU part from the
  base to what is used and then the GPU part. Runs of three or more zero
  words are collapsed: a10's 17,986,420 bytes of state become 0.41 to
  0.73 MB.
- **No native pointer.** A pointer into the game state is written as
  `0x40000000` plus its offset, one into the tag cache as `0x80000000` plus
  its offset there, and none as 0. That covers:
  - each data array's pointer to its elements;
  - each memory pool's pointers to its room and its first and last
    blocks, and each block's pointers to its neighbours and its reference;
  - each lruv cache's pointer to its blocks;
  - in each script thread in use, the pointer to its top frame and each
    frame's pointers to its previous frame and its result;
  - the fields the owning units declare (below).

  A block's reference (the element's field that points to the block) is
  written as 0 and made again from the block. An lruv cache's procedures
  are written as 0 and restored from the build.
- **Byte order.** A field narrower than 32 bits keeps its place within its
  word of the port that wrote it. The header says which byte order that
  was, and an image from the other one is refused. Writing each field by
  itself is format 3 (HWI-046).

### The pointer census

Which words of the game state are pointers was measured, not assumed:

1. **Debug information.** Every pointer member of every type the game
   state allocates, walked recursively through structures, unions and
   arrays in the Wii engine's DWARF. Besides the structures above:
   - a recorded animation's place in its event stream (tag cache);
   - the HUD messages' state messages (tag cache), and an icon-or-text
     union beside them;
   - the HUD's help and objective messages (tag cache);
   - a glow's head and tail particles and each particle's links (game
     state);
   - an effect's impulse procedures and their data (always none);
   - the cinematic screen effect's three bitmaps (tag cache; not made on
     the Wii);
   - the detail objects' per-player caches, which `structure_detail_objects_flush`
     marks for rebuilding after every load.

   Each owning unit now declares its fields
   (`game_state_port_data_pointer_field` for a data array's elements,
   `game_state_port_pointer_field` for a global's words). The detail-object
   caches are not declared: they are rebuilt before they are read.
2. **Two placements.** The same tick of a10 (1, 30, 150, 400, 1,000,
   2,000, 3,000, 4,500, 6,000, 8,000 and 8,999) and of Prisoner (1 and 299)
   was saved at arena offsets 0 and 4,096, and every word that differs
   between the two images was classified. Before the declarations, live
   words moved with the arena from tick 400 on: the recorded animations'
   and the HUD's tag pointers. After them, every differing word lies in room
   nothing reads:
   - a memory pool's room outside its blocks (old copies of moved blocks);
   - a data array's elements not in use (`datum_new` clears one before it
     is used again);
   - a script thread's stack above its top frame.

   Those words are carried as they were, as the original path carried
   them. At the same base they are what the original restored; at another
   base they hold the other base's addresses, which nothing reads.

**A finding beside the census.** a10's 9,000-tick simulation digest is
`f752db70` at arena offset 0 and `03181654` at +4 KiB (HWI-029's
correction). Yet the two placements' images at tick 8,999 have identical
live words, the same random seed, object count and player position. The
harness's canonical digest reads any word that falls in an arena region as
a pointer, and datum handles whose salt falls in that range (0x904d to
0x918d on these placements) are read as pointers too. The difference is the
digest's, not the game's. For HWI-029/HWI-030 to confirm.

## Saving and loading

**Save** (`game_state_image_write`):
1. The header is written first, not marked complete.
2. The state's words are encoded into one chunk buffer, each full chunk
   written with its CRC.
3. The header is written again, complete, with the SHA-256.

A save cut short leaves an incomplete header, which is refused. The file
then ends where the image does.

**Load** (`game_state_image_load`) reads the file twice and holds one chunk
at a time:
1. **Check.** Nothing in the game state changes. The pass checks:
   - the header's CRC, version, completeness, byte order and encoding;
   - the layout and allocation checksum against this build's;
   - the map, build, player count and map checksum against the game's;
   - every chunk's index, size and CRC, and the segments and the length;
   - the payload's SHA-256;
   - every pointer's place and every data array, memory pool, block and
     script thread, as upstream's `game_state_image_accept` checked the
     image in memory.
2. **Take.** Only then the before-load procedures run and a second pass
   reads the file into the game state, making the pointers again at its
   base. Each chunk's CRC must be the one the first pass checked, and the
   header must be the same. Block references are made again from their
   blocks.

A file that read differently the second time has already changed the game
state. The load then says so, and the game halts with the original's
message ("game state has been corrupted"). It cannot be refused cleanly
without a second buffer. No run reached it.

**What a refusal does:**
- **Revert or loss:** the game state is left as it is and, as before, the
  map starts over. The refusal no longer halts first.
- **Core load:** "couldn't load", the game state left as it is. Before, a
  core whose header did not match halted the game in
  `game_state_header_valid`; it is now refused with the reason in the log.
- **"Save and quit" resume:** not loaded, and the file deleted, as before.

**Checkpoint semantics are unchanged:** what is saved, when, and what a
revert restores. A revert restores exactly the bytes the checkpoint saved.
Only the file's form changes.

## Host proof (i686)

### Equal to the original saved-game path

The same plans ran on two host references built from the same tree: the
original path (`wii` at `bae56603`) and the canonical image. Their report
lines (RUN, ALLOC, TICKS, TICKS_RAW, EXPECT, FIXED_STEP, MAP_RUNS, LOAD and
LOAD_FAILURE) were compared in order, timings left out:

| Plan | Lines | Equal |
|---|---:|---:|
| HWI-034's scenarios: revert, loss and core load at six cadences | 1,592 | 1,592 |
| HWI-034's evidence plan, replayed as its Titan launch 1 | 4,258 | 4,258 |
| a10, 9,000 ticks at six cadences | 1,392 | 1,392 |

| Line | Request | Simulation, every cadence | Against the 30 Hz run |
|---|---|---|---|
| `a10revert` | `game_revert` after tick 51 | `e825c57b` | every tick equal |
| `a10loss` | `game_lost` after tick 212; revert due after 303 | `ade62b23` | every tick equal |
| `a10core` | `core_load` after tick 54 of a core saved after tick 30 | `9453ab30` | every tick equal |

No tick began after a request in its frame, and every tick the clock gave
ran.

**Regressions, unchanged:**
- a10 `f752db70` at every cadence (`03181654` at +4 KiB, above);
- Prisoner `127656ef` / `961a5ec5`, whole state `7eb53576`;
- HWI-015A's scenario `c1ad6ae6`.

No whole-state digest moved: the canonical image restores exactly the
bytes the original restored.

Per load (18 in the scenario plan), the state after the load equals the
state at the save (`same_as_saved=1`), and writing it again gives the same
SHA-256 (`reencoded_same=1`).

### Damaged images

The `a10revert` line saves a core after tick 40 at each of its six cadences
(`image_test=40`), checks it whole (valid), then makes twelve damaged copies
and loads each through `game_state_read_core`, the core load's own path:

| Case | Damage | Refused as |
|---|---|---|
| `flip_payload` | one payload byte flipped | `chunk_checksum` |
| `flip_payload_checksum_fixed` | the same, the chunk's CRC made again | `digest` (SHA-256) |
| `flip_header` | one map-name byte flipped, header CRC kept | `header_checksum` |
| `truncated_half` | cut in half | `read` |
| `truncated_last_byte` | one byte short | `read` |
| `empty` | no bytes | `read` |
| `wrong_map` | another map's name, header CRC made again | `map` |
| `wrong_build` | another build number | `build` |
| `wrong_version` | version 3 | `version` |
| `incomplete` | the completion flag cleared | `incomplete` |
| `wrong_byte_order` | the other byte order | `byte_order` |
| `wrong_layout` | another allocation checksum | `layout` |

At all six cadences, each case was refused for its reason, and the CRC of
the game state's whole 20 MiB room was the same before and after the load.
The run's ticks are the same as without the test: a refused image changes
nothing.

**A core from the Wii, loaded on the host.** The core a Wii launch saved
was put on the host's card, and the plan's `core_load` asked for it. It was
refused: its header's allocation checksum is the big-endian one. The run's
report lines equal those of the same plan with no core on the card, 530 of
530, at six cadences.

### Rebinding at another base

| Core | Saved at | Loaded at | Load writes back | After the load |
|---|---|---|---|---|
| tick 30 | offset 0 | 4,096 | the same SHA-256 (`d0e33765…`) | raw chain `8e12e360`, the native 4,096 run's |
| tick 8,000 (a recorded animation's tag pointer in it) | offset 0 | 4,096 | the same SHA-256 (`d146871b…`) | same seed. At tick 8,199 the two states differ only in dead room and in one script frame's `result` field |

The `result` field of a frame pushed after the load is not written by the
push (`hs_stack_push`). It keeps the stack's stale bytes, here an address
of the saving placement, until the frame evaluates a sub-expression and
writes it, which comes before any read.

## Dolphin

The evidence plan (`map.txt`) has 17 lines: HWI-034's 14 and its three
blocked scenarios, `a10revert` (with `image_test=40`), `a10loss` and
`a10core`, each at all six cadences. A plan may now hold 20 lines. Each
launch's host reference runs the same plan at the launch's arena offset (0,
then 4,096) and replays the launch's measured retrace frame times. The
runner read back the card's checkpoint and core: the independent Python
reader (`tools/wii/game_state_image_format.py`) finds them valid, byte
order 2 (big-endian).

| Launch | Guest | Plan | Run lines equal to the host | Image events equal to the host | Persistence | Host process |
|---|---|---|---|---|---|---|
| Research 1 | `END result=pass` | 12/12 expectations | 63/63 | 227/227 | passed | **`0xfffffffb`** (signal 5) after the guest's `END`, 1,654.5 s |
| Research 2 | `END result=pass` | 12/12 | 63/63 | 227/227 | passed | `0x0`, 1,654.4 s |
| Titan 1 | `END result=pass` | 12/12 | 63/63 | 227/227 | passed | `0x0`, 1,658.8 s |
| Titan 2 | `END result=pass` | 12/12 | 63/63 | 227/227 | passed | `0x0`, 1,656.8 s |

Every launch shared its host with another lane (`host_shared=true`).

- **Revert, loss and core load on the Wii.**
  - Every cadence gives the line's 30 Hz simulation at every tick, and each
    equals the host's at every tick.
  - Frames stopped at the request at 20 Hz, at irregular pacing and in the
    retrace-paced runs. No tick began after a request in its frame, and
    every tick the clock gave ran.
  - Launch 1 (offset 0): `e825c57b`, `ade62b23`, `9453ab30`, the original
    path's values on the host. Launch 2 (+4 KiB): `527450c2`, `17b7be9a`,
    `9453ab30`, the host's at that offset (the canonical digest's
    placement artifact, above).
- **Images on the Wii.** In each launch:
  - 53 saves and 18 loads taken;
  - every load gave back exactly the saved state and writes back the same
    SHA-256;
  - the image test passed at all six cadences (12/12 refused, state
    intact);
  - 72 damaged loads were refused, and none failed while taking.

  The Wii's payloads are the host's byte counts exactly: 458,652 to
  732,152 bytes, 7 to 12 chunks.
- **Ticks compared with the host:** for each line's first run, a10 9,000,
  `a10sw` 600, `a10bsp` 10, Prisoner 900, and `a10won`, `a10reset`,
  `a10revert`, `a10loss` and `a10core` 400 each. None differ. 4,134
  allocation checkpoints compared per launch.
- **Between hosts.**
  - Launch 1: all 63 run lines identical on Titan and Research.
  - Launch 2: 62 of 63. In `a10revert`'s retrace-paced run one measured
    frame time differed between the hosts. The simulation chain is the
    same; the whole-state chain, which holds the frame-coupled
    allocations, differs. Each host equals its own replay.
- **Regressions on the Wii:**
  - Prisoner `127656ef` / `961a5ec5`;
  - a10 `f752db70` (launch 1) and `03181654` (launch 2, +4 KiB);
  - HWI-015A `c1ad6ae6` / `58ce9719`.
- **OS stability:** no instability events on either host. Research's
  launch 1 fault came after the guest's `END`, and launch 2 read the card
  back intact. This is the shutdown-only Dolphin fault class ADR-017 defers
  to HWI-072, reported here, not tolerated away.

**Preliminary launches.** Before the census found the tag-cache pointers,
two launches per host ran from clean `da5cfce3` (build `8b27600da127af5b`)
with the same plan. Both passed, 63/63 run lines equal to the host's.
Research launch 2 exited `0xfffffffb` after the guest's `END`. They are not
the evidence build.

**Focus.** These batches ran with the runner of their own tree, from before
HWI-077's focus-safe runner (merged into this branch afterwards). Research's
launches may have taken focus on the Studio. Later batches run from a tree
with HWI-077.

## Memory

| | Before | Now |
|---|---:|---:|
| A second buffer for a load | 20,971,520 bytes (`malloc`, failed on the Wii) | none |
| The image code's peak heap, saving | 20 MiB file written from the state | 71,684 bytes: the 65,552-byte chunk and its header, a 4,096-byte segment buffer, the pool's block references, one element |
| The image code's peak heap, loading | 20,971,520 bytes | 66,188 bytes: the 65,536-byte chunk, the chunks' CRCs, one element |
| A checkpoint on the card (a10) | 20,971,520 bytes | 459,276 to 732,856 bytes |
| MEM2 spare after the arena plan | 2,985,984 | 2,985,984 (unchanged) |

On the Wii:
- the arena was restored at the close and the heap was flat (2,126,040
  bytes in use, as in HWI-034);
- the main stack's peak was 105,760 bytes (HWI-034: 105,600);
- MEM1 free at entry was 2,951,392 bytes.

The image code itself (`game_state_image.o`) is 20,790 bytes of code and 116
of zero-initialised data. The harness's reporting and tests
(`save_image_scenario.o`) are 7,764 and 5,272. `game_state.c` keeps the
layout table for the image (12,288 bytes) and the declared pointer words
(512 bytes).

## Storage and format versions

- **Where.** As before: the checkpoint at `z:\savegame.bin`, which is
  `sd:/halo-wii-engine/saves/z/savegame.bin` on the emulated card; cores in
  `d:\core\`, which is `sd:/halo-wii-engine/data/core/`. The files are no
  longer padded to 20 MiB. SD versus NAND, profiles and migrations stay in
  HWI-046.
- **Versions.** Version 1 is the upstream image: the game state's memory,
  with no header. It is refused (magic). Version 2 is this format.
  - A layout change (a pool's size) or another map, build, player count or
    byte order is refused by the header.
  - Migration (HWI-046): a version 3 reader takes a version 2 image of its
    own byte order through this decoder and writes it again. Version 3 is
    to add per-field encoding (byte-order independence), write generations
    and a backup.
  - A version 1 image could be converted offline only at its own base, the
    native ports' fixed address.

## Not covered

- **Byte-order independence.** An image is taken only by a port of the
  same byte order: narrower fields keep their place in their word. This is
  format 3 (HWI-046).
- **Cross-port play with images** (a Wii save on the PC): refused (above).
- **Dead room holds the saving placement's addresses.** Nothing reads it
  before writing, but a cross-placement load is not byte-identical to a
  native one there.
- **Interruption.** A save cut short loses the previous checkpoint, as the
  upstream in-place write did. Write generations are HWI-046.
- **"Save and quit"** is converted but not exercised by a scenario (the
  harness has no profile).
- **Network games** are not exercised, as in HWI-034.
- **Physical Wii:** untested.

## Timings

| Step | Host | Wall time |
|---|---|---|
| Host reference build, full | Titan (WSL Debian clang) | 67 s (canonical image); 80 s (original path, `bae56603`) |
| Host reference build, incremental | Titan | 13–20 s |
| Host proof: HWI-034's evidence plan | Titan | 72 s (canonical); 113 s (original) |
| Host proof: three scenarios at six cadences | Titan | 26 s (canonical); 29 s (original) |
| Host proof: a10, 9,000 ticks at six cadences | Titan | 75 s (canonical); 80 s (original) |
| Cross-placement census: 26 host runs | Titan | 302 s |
| DWARF listing (332 MB) and the pointer walk | Titan | 15 s and 6 s |
| Host reference of one evidence launch | Titan | 96–134 s |
| Wii build: `ninja wii_engine` | Titan | 64 s (first in this tree after configure); 120 s for the evidence build |
| Two cold launches, shared host | Research, Dolphin Vulkan | 3,326 s (1,654.5 s and 1,654.4 s). Load average 2.48 3.12 3.15 before, 3.32 3.85 3.45 after |
| Two cold launches, shared host | Titan, Dolphin D3D | 3,380 s (1,658.8 s and 1,656.8 s), after waiting for a slot |
| Preliminary launches (`da5cfce3`) | Research / Titan | 3,318 s / 3,346 s |

The guest's own elapsed time is 1,653.5–1,653.9 s per launch (HWI-034:
1,296.1 s for 14 lines). On the Wii (guest time):
- a save takes 0.59–0.63 s;
- a load that is taken takes 0.25–0.26 s, both passes;
- a refused one ends where it is refused (from 0.3 ms);
- the harness's own checks after a load take 0.80 s: the CRC of the 20 MiB
  room and writing the state again.
