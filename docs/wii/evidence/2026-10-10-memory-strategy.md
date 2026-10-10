# Memory strategy: measured map, relocation and game-state ownership (HWI-007)

This run measures the Wii memory map and what fits of the current upstream
reservations. It then exercises relocation on one owned map's whole tag graph
and on the full upstream game state, in stock Wii-mode Dolphin.
[Numeric evidence](2026-10-10-memory-strategy.json)

**Result:** relocation with typed boundaries works for both the tag cache and
the game state. The simulation state fits MEM2 at the full upstream
capacities. The complete upstream build does not fit as it stands:

- **Measured:** neither bank can hold a texture cache of the Xbox size next
  to the required reservations.
- **Estimated:** the engine image as the Linux build compiles it would leave
  under 1 MB of MEM1.

## Build

| Item | Value |
|---|---|
| Source | Clean `03d28d6c` (engine change `2b7a3745`, see below) |
| Build ID | `a96965edd20918c2` |
| Toolchain | devkitPPC GCC 16.1.0, binutils 2.46.0, strict C11 |
| `memory_strategy.dol` | 482,656 bytes, SHA-256 `892ad40ba15e246ae921256fce8e710eeb4ac19bc4282a60391a2fcdfdd98156` |

Engine units are the actual `source/memory/data.c` and `memory_pool.c`. Per
ADR-018 they build with `-fsigned-char`, and the guest clears FPSCR NI before
any engine code runs. FPSCR is `0x4` at entry and `0x0` after the clear. No
game data is embedded.

The guest reads private tags and one structure BSP from the emulated SD card,
the same inputs as the geometry evidence: 16,995,988 and 1,288,192 bytes. A
manifest on the card supplies the host reference's expected values.

## Result: two cold launches

Both launches gave the same results.

| Check | Each launch |
|---|---|
| Load/use/unload cycles | 4 of 4 pass. Placements alternate by 4 KiB. |
| Whole-map graph report, encoded walk | Equals the host reference (i686 and x86-64) line for line, every cycle |
| Relocated native walk | Same digest `03a00f90` and identical report, every cycle |
| Game-state images | 8 of 8 CRCs equal the i686 host. Launch 2 restored launch 1's last image. |
| Stale handles | Arena handles reject after release, 4 of 4. Every datum deleted by an update (9 or 10 each) rejects. |
| Tag faults | Block address past the tags and a tag-table datum both reject at the expected offset. The restored bytes then walk cleanly. |
| State faults | Truncated, flipped bit, bad magic and empty images reject. The region stays unlive. |
| MEM2 arena | Restored after every cycle |
| Heap use across cycles | Flat at 7,230,408 bytes, including the 5.57 MB graph workspace |
| Launch counter read back | 1, then 2 |
| Host exit | 0x0, natural, about 22 s per launch |

No System event 41, 1001 or 6008 and no Application error naming Dolphin were
logged during the session.

## Measured memory map (Dolphin, IOS 58 rev 6176)

| Region | Bytes | Notes |
|---|---:|---|
| MEM1 arena at `main` | 24,312,896 | `0x800d03c0..0x81800000`. This image ends at `0x800c03c0`; libogc took 65,536 before `main`. |
| Main and exception stacks | 131,072 + 16,384 | In BSS. The main-stack peak below `main`'s frame is 1,560 bytes for this diagnostic, not for the engine. |
| GX FIFO and two XFBs | 262,144 + 2 × 614,400 | The heap is 1,659,832 in use after GX init. MEM1 left: 22,634,496. |
| MEM2 arena at `main` | 54,386,688 | `0x90002000..0x933e0000`. IOS holds 12,713,984 above it. |
| MEM2 after video/GX init | 54,368,224 | The arena top lowered by 18,464 |

The geometry diagnostic also initialises PAD and WPAD. It recorded its arenas
after that initialisation: 243,904 bytes of MEM1 and 102,560 of MEM2 below
the link-time bounds.

**Heap behaviour.** A greedy probe of 1 MiB, then 64 KiB, requests reached
22,085,632 bytes in MEM1 and 54,263,808 in MEM2. Once the libogc heap moves
to MEM2 it does not come back: in one development run, MEM1 became
unreachable through `malloc` after a single 32 MiB request. After the move,
`mallinfo` counts the 232 MiB gap between the banks as heap: `arena` is
267,594,816 and `uordblks` 245,482,440 after every block was freed. Neither
measures peak or fragmentation once that has happened.

## What fits of the upstream reservations

| Plan in the measured arena | Requested | Result |
|---|---:|---|
| MEM2: tags 22 MiB + game state 20 MiB + sound 4 MiB + Xbox textures 22 MiB | 71,303,168 | Short by 16,934,944 |
| Same, with the desktop texture cache (256 MiB) | 316,669,952 | Short by 262,301,728 |
| MEM2: tags + state + sound + two 64 KiB IO buffers + 2 MiB reserve | 50,462,720 | Fits, 3,905,504 left |
| Same, with the state sized to the census | 47,695,936 | Fits, 6,672,288 left |
| MEM1: Xbox texture cache after GX, with this 0.5 MB executable | 23,068,672 | Short by 434,176 |

**Game-state census (measured).** The tool lists all 72 upstream allocation
sites (75 rows, 81 allocations). The compiler evaluates each site's own count
and size expressions on the upstream i686 ABI. The CPU part requests
18,040,844 of its 20,709,376 reserved bytes, and the GPU part 163,840 of
262,144. The largest requests:

| Pool | Bytes |
|---|---:|
| Object pool | 8,388,664 |
| Props | 2,555,960 |
| Actors | 1,871,928 |
| Particles | 917,560 |

On PPC, every one of these allocations was made in the 20 MiB state slot with
the engine's `data_initialize` and `memory_pool_initialize`, at the upstream
capacities.

**Engine image (estimate, not measured on PPC).** This sums the 500 game units
the Linux build compiles. Each was compiled alone for the upstream i686 ABI;
nothing was linked.

| Section | Bytes |
|---|---:|
| `.text` | 2,688,160 |
| `.rodata` | 692,476 |
| `.data` | 394,823 |
| BSS | about 18.57 MB |

Most of the BSS comes from four sources:

| Source | BSS |
|---|---:|
| Custom Edition map support | 6.62 MB |
| The tag validator's claims bitmap | 3.01 MB |
| Network and co-op buffers | 4.33 MB |
| Profiling | 1.13 MB |

Linked as is into MEM1, the estimate leaves under 1 MB for the heap and any
texture cache. PowerPC code is usually larger than i686 code, which this
estimate does not include.

## Whole-map tag graph and relocation

`tools/wii/export_tag_schema.py` turns the upstream validator's schema into
plain data with i686 offsets: 349 definitions, 1,153 fields and 78 groups.
Nine pointer fields are added that the validator's checks read rather than
its typed fields:

- the model part and BSP material buffers;
- the scenario's BSP load addresses.

`cache_schema_graph.c` walks the tag table, every root and every block. It
then walks the loaded BSP. It reads each number explicitly little-endian and
reproduces the validator's claims and bounds without correcting anything.

| Count | Value |
|---|---:|
| Tags | 3,647 |
| Roots walked through a schema | 3,633 (2 groups have none) |
| Blocks | 21,430 (227,472 elements) |
| References | 16,879 (7,902 resolved) |
| Tag indices | 1,799 |
| Reference and tag-index edges | 9,701 |
| Tags reachable from the scenario | 3,277 |
| Bytes claimed | 12,869,918 |
| Validator corrections needed | 0 |

The representative chains are all inside the walk:

- scenario → BSP (13 edges), BSP → `senv`/`sgla`/`sotr` shaders (22/15/14)
  and bitmaps (292);
- scenario → weapon (11), weapon → model (15), animation (9) and collision
  (7), model → `soso` shaders (183);
- biped and vehicle → model and animation.

Relocation rewrites 39,224 pointer fields to their native MEM2 addresses,
including 3,037 buffer pointers read by checks. It sets 77,470 empty or
invalid ones to null, as the validator does. A walk through the native
pointers gives the identical report. After relocation, the header's tag
table and the scenario root are read directly as native MEM2 pointers:
`0x90002024` and `0x90542b6c` at placement 0, `0x90003024` and `0x90543b6c`
at placement 1.

In Dolphin, a walk takes 0.73 s, relocation 0.76 s, and loading 18 MB from
the emulated SD card 1.49 s. These are emulator timings.

**Residual audit.** 17,868 aligned words lie in the tag slot's encoded
address range but are named by no pointer field:

| Where | Words | Notes |
|---|---:|---|
| Unclaimed bytes | 13,854 | Vertex payload and other data no schema field claims |
| Claimed, unaligned values | 2,685 | Data |
| Claimed, aligned values | 1,329 | Mixed: see below |

The aligned claimed words mix three kinds:

- script datums whose salt happens to fall in `0x803a..0x819a`;
- packed BSP data;
- real pointers in blocks the game never reads. Two examples are game
  globals' playlist and a particle system's physics constants, which the
  schema leaves out.

Rewriting every address-like word would therefore corrupt datums.
Classifying these words field by field is still open.

## Game-state ownership

`game_state_image.c` makes every census allocation in a region at any base.
It drives representative players, bipeds, weapons, vehicles, scenery, actors
and props through the engine's own calls:

- `datum_new` and `datum_delete`;
- `memory_pool_block_allocate`, `memory_pool_block_reallocate` and
  `memory_pool_block_free`;
- `memory_pool_compact`.

It saves a canonical little-endian image with no native pointer in it:

- data-array headers without their element pointer;
- live elements only;
- pool blocks in order, each with its owner (allocation, element, field);
- element pool references stored as block ordinals.

Restore rebuilds the region at another address and rebinds every pointer. It
checks:

- the allocation checksum;
- every count against its capacity;
- that block owners are unique;
- the trailing CRC;
- the engine's `data_verify`.

Images are 284 to 286 KB. A restore takes 44 ms and a save 49 ms in Dolphin.
The PPC images are byte-identical to the i686 host's for all 8 steps across
both launches.

**Datum identifier seeds.** `data_delete_all` seeds each array's identifiers
by copying two name characters into a short. On PPC that copy reads them the
other way round in all 36 data arrays: `object` gives `0xef62` instead of
`0xe26f`. The distributed netcode names objects by full datum index, so a
Wii peer would disagree with every other port. Commit `2b7a3745` composes
the seed in little-endian order. Little-endian builds compute the same value
as before, and after the change PPC matches the host.

## Candidate strategies against these measurements

| Candidate | Finding |
|---|---|
| Relocation with typed boundaries | Demonstrated for the tag graph at two MEM2 bases and for the game state across bases and launches. Costs one load-time pass, 0.76 s in Dolphin. Open: the residual classification and the endian conversion. |
| Limited fixed-address region | Rejected. The upstream bases `0x803a6000` and `0x81a00000` are outside the usable arenas (ADR-016). A fixed MEM2 base still needs every cache pointer rewritten, because the caches are linked to `0x803a6000`. Saves would also keep absolute pointers. |
| Virtual mapping | Not demonstrated, and not needed for addressing. The tag range starts inside MEM1, under the executable's own mapping. GX and IOS translate addresses by masking, not through a page table. |

The upstream capacities stay as they are. All 81 game-state allocations fit
the 20 MiB slot, and the state reservation alone could shrink to the
census-measured 18.2 MB without changing any capacity. The open budget
question is MEM1: the executable's code and BSS against texture residency.
The texture cache is a cache, so its size is a presentation budget, but the
per-frame working set of retail maps is unmeasured.

## Development iterations (retained)

**dev-1** (dirty). The guest passed, but the runner compared the first
cycle's full report with later cycles' summary lines. The runner was fixed.

The same run's binary search for the largest `malloc`, made after the cycles,
reported a 67,043,328-byte request as successful. That is larger than either
bank's free arena. This is unexplained. The search was replaced by fixed
requests: 16 MiB lands in MEM1, 32 and 48 MiB in MEM2, and 60 MiB fails.

**dev-2** (dirty). With those fixed requests made first, the heap moved to MEM2 and
the greedy probe then reached no MEM1 memory. The probe order was swapped.

## Not established

- Tag fields are not byte-swapped. The relocated graph proves addressing,
  not endian conversion (HWI-008).
- Engine element layouts on PPC are not measured. The census compiles only
  on the i686 ABI. Engine headers do not yet build against newlib: two
  attempts were made and stopped, and this belongs to HWI-015.
- Only one map and one BSP were walked.
- The residual words are not classified field by field.
- The game-state payloads are representative, not engine objects. The lruv
  cache and GPU decal vertices are rebuilt empty.
- No texture or sound decoding was done, and no per-frame texture working
  set was measured.
- No engine stack peak was measured.
- The engine image size is an i686 estimate.
- Physical Wii is untested.
