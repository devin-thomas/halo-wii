# Campaign BSPs drawn within memory (HWI-030, formerly HWI-016E)

HWI-026 ran the campaign map a10 equal to the i686 host with drawing off.
With drawing on, its first BSP could not be drawn: loading the BSP's HWL1
geometry file beside its decoded form needed 8,100,736 bytes against
3,575,808 free above the reserve (short 4,524,928), and the largest BSP
11,664,352 (short 8,088,544).

The Wii rasterizer now draws each BSP's geometry where the staged map
already holds it for the game, in the tag slot. Geometry costs no memory
beyond the tag slot, and textures fill one render pool per map, refilled at
every BSP switch. a10 draws through its script-driven BSP switches
(0→7→1→8→0) and through labelled script switches into every other BSP, and
its game state equals the i686 host at every tick with drawing on.
[Numeric evidence](2026-10-11-campaign-render.json)

**Result:**

- **Memory:** all 9 of a10's BSPs are drawn. Geometry owns 0 bytes; the
  tag slot has 341,840 bytes left at the largest BSP (BSP 6). Each BSP's
  required textures (material tables and every lightmap page) leave at
  least 1,029,216 bytes of the 2,711,552-byte pool for base maps (BSP 6;
  1,025,120 in the second launches, whose pool is 4 KiB smaller).
- **What is drawn is unchanged:** at every BSP load the tag slot's
  vertices, lightmap vertices and surfaces equal the HWL1 file byte for
  byte (67 of 67 loads per launch). Prisoner's five fixed poses give
  HWI-008F's frame CRCs exactly, on both hosts.
- **Game state:** with every frame drawn, all 28 run lines equal the i686
  host in every launch, whole state included: 1,794 allocation checkpoints;
  a10 9,000 ticks, `a10sw` 800 ticks (eight labelled switches covering every
  BSP), `a10bsp` 10, Prisoner 900. Every pose leaves the game state's digest
  unchanged.
- **Found and fixed (engine, all ports):** the door logic read a damage
  flag through a byte offset, so the Wii tested bit 10 where the Xbox
  tested bit 2 and opened a door the Xbox kept shut. a10 diverged from the
  host 4 ticks after a switch into BSP 5. It now reads the field as a value.
- **Regressions:** Prisoner `127656ef` / `7eb53576`; a10 `f752db70` at
  30 Hz, 60 Hz and retrace pacing (at arena offset 0; `03181654` at
  +4 KiB, as in HWI-029, equal to the host there); HWI-015A `c1ad6ae6`.
- **Textures, escalated:** under the provisional 3 MiB base-map budget
  alone, 8 of 9 BSPs would texture over 99.9% of their base-mapped
  surfaces. The pool left beside the lightmaps holds less: per BSP 7 to 10
  base maps (all 8 of BSP 8's). Covering every BSP to the budget needs up
  to 1,965,824 bytes more (BSP 6; 1,969,920 at the +4 KiB placement).
  Not cut: the rest draws lightmap-only, counted. The texture cache is HWI-054's.
- **Hosts:** two cold launches each on Titan (D3D) and Research (Vulkan),
  guest `END result=pass`, persistence passed, host exit `0x0`, no OS
  events. Physical Wii untested.

## Build

| Item | Value |
|---|---|
| Source | Clean `125e0132` on `agent/hwi-016e` (merges `wii` at `6d898c7c`), `source_dirty=false` (HWI-027 rule) |
| Build ID | `f07fac9f15d54881` |
| Toolchain | devkitPPC GCC 16.1.0, binutils 2.46.0 |
| `engine.dol` | 4,184,992 bytes, SHA-256 `eabeaeaece734abc69a6d3a0f4effbaae1015bd2e31bb0037a93d9c31fb4b18f` |
| `engine.elf` | 37,820,992 bytes, SHA-256 `884f762a0a2d5aa89d0ca0636e15a22f2fbabb31dcf2a790fe4029ad96f55208` |
| Host reference | i686, Debian clang 19.1.7, the same engine units; program SHA-256 `60d74ff3805e95465193bb7c3ce44717e4255ffdc7c3078abc6ebccdbd97605c` |
| Maps | HWI-026's HWIMAP02 stagings, unchanged |
| Render files | Converter generation `6e7b87372c08e3fa` (`gx-splitalpha-v1`), staged by `render_stage.py`: a10's 9 HWL1 files, 339 textures (private) |
| Artifacts | `package_artifacts.py` stage and inspect `--require-clean`: 33 files, 0 problems, 0 findings; `engine_unsupported.py --check` passes |
| Tests | 528 on Python 3.14 and 3.11 |

## Geometry: the options measured

Bytes each option holds for one BSP beyond what a10 holds with drawing off,
at the load's peak and while the BSP is drawn. Computed from the staged
HWL1 files with the loader's own sizes (the HWI-016B path's two figures are
the ones HWI-016C measured on the Wii). Every option releases a BSP's
geometry on a switch, so the peak is one BSP's.

| Option | First BSP (0) peak / drawn | Largest (6) peak / drawn | Fits 3,092,480 free? |
|---|---:|---:|---|
| HWL1 decoded whole (HWI-016B), then reduced | 8,100,736 / 1,866,368 | 11,664,352 / 2,677,632 | no |
| Normals kept packed (11:11:10), file beside it | 5,946,400 / 2,973,120 | 8,563,424 / 4,281,632 | no |
| HWL1 decoded in chunks straight into GX arrays (24 B a vertex, 6 a triangle) | 1,899,136 / 1,866,368 | 2,710,400 / 2,677,632 | geometry only; leaves 382,080 for lightmaps (BSP 6 needs 1,547,648) |
| GX-ready form staged at import (normals not resident: the path reads none) | 1,866,368 / 1,866,368 | 2,677,632 / 2,677,632 | as above |
| Lending the sound region (4,194,304, unused while sound is not started: HWI-016C) | — | — | would hold either of the two above, until sound starts |
| **Drawn from the tag slot (chosen)** | **32,768 / 0** | **32,768 / 0** | **yes** |

"Free" is MEM1 106,496 + MEM2 2,985,984 above the 2 MiB reserve after
engine start-up, in this build. The 32,768-byte peak is the HWL1
comparison's read chunk, taken from the render pool and given back.

**Why the tag slot.** The staged map already holds each BSP material's
`compressed_vertex_data`, big-endian (`map_stage.py`): 32-byte environment
vertices (position F32 at 0, texcoord F32 at 24) and 8-byte lightmap
vertices (texcoord S16 at 4), and the BSP's surfaces (three u16 each). Those
are GX's indexed arrays as they stand; the engine's
`(2s + 1) / 65535` lightmap texcoord is in the texture matrix. Nothing is
decoded or copied, so a BSP switch costs only what the engine's own switch
reads into the slot. The HWL1 file is the same bytes converted by the
content pipeline; at every BSP load it is read in 32 KiB chunks and
compared byte for byte, and a BSP whose bytes differ would not be drawn
(`RENDER_GEOMETRY_CHECK`). The packed normals, binormals, tangents and
incident radiosity directions stay in the slot as staged; the environment
path reads none of them (`test_render_residency.py`), and HWI-054's
lighting can read them there.

**Unchanged output.** At equal residency the frame is the one HWI-008F drew
from the decoded HWL1 form: Prisoner's 5 fixed poses give Titan
`62ff41b0` `f815b5a5` `2fd595e3` `76e27f63` `acf4671e` and Research
`1de20107` `4e99f685` `e3eb3c53` `a6357304` `972ab0b9`, HWI-008F's full
CRCs exactly.

## Memory per BSP

From the a10 run's BSP loads (Research launch 1; Titan and launch 2 are
the same but where noted). The tag slot holds the tags (14,158,000 bytes)
and the BSP loaded, of 23,068,672. The render pool is MEM2's 2,711,552
bytes above the reserve and a 262,144-byte keep (MEM1's 69,632 free bytes
at the map's load are under its 65,536 keep plus slack, so its part is
empty).

| BSP | Tag | Surfaces | Geometry drawn (in the slot) | Slot left | Lightmap pages | Required: tables + lightmaps | Pool left for base maps | Base maps resident | Surfaces textured | Load (ms, emulated) |
|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---:|
| 0 | 3263 | 40,946 | 2,938,644 | 2,680,656 | 30 (956,288) | 1,033,312 | 1,678,240 | 10 / 12 | 40,842 / 40,934 | 148 |
| 1 | 3285 | 54,908 | 3,909,264 | 655,184 | 38 (1,436,032) | 1,539,328 | 1,172,224 | 8 / 13 | 53,929 / 54,800 | 223 |
| 2 | 3298 | 40,642 | 2,891,972 | 2,852,688 | 36 (1,125,632) | 1,227,680 | 1,483,872 | 9 / 14 | 39,841 / 40,642 | 226 |
| 3 | 3307 | 36,544 | 2,619,344 | 3,438,416 | 38 (1,010,048) | 1,113,792 | 1,597,760 | 9 / 14 | 36,306 / 36,544 | 227 |
| 4 | 3309 | 52,128 | 3,762,808 | 1,156,944 | 50 (1,396,864) | 1,514,176 | 1,197,376 | 8 / 12 | 49,394 / 52,088 | 285 |
| 5 | 3313 | 56,668 | 4,135,248 | 378,704 | 47 (1,297,344) | 1,418,048 | 1,293,504 | 10 / 19 | 53,055 / 56,648 | 302 |
| 6 | 3323 | 57,532 | 4,221,312 | **341,840** | 54 (1,547,648) | 1,682,336 | **1,029,216** | 7 / 13 | 54,128 / 57,532 | 356 |
| 7 | 3325 | 8,190 | 570,876 | 7,380,816 | 7 (1,311,168) | 1,334,752 | 1,376,800 | 7 / 17 | 4,683 / 7,808 | 111 |
| 8 | 3351 | 17,216 | 1,248,952 | 6,149,968 | 9 (1,081,920) | 1,093,216 | 1,618,336 | 8 / 8 | 17,192 / 17,192 | 112 |

- **Margins, exactly:** the tightest tag slot is BSP 6's, 341,840 bytes
  (as HWI-016C measured with drawing off: drawing adds nothing there). The
  tightest pool is BSP 6's, 1,029,216 bytes after its required lightmaps.
  The heap keeps its 262,144-byte MEM2 keep: the least MEM2 above the
  reserve any frame or load saw is 266,240 bytes (229,376 in each
  first launch: the heap grew 36,864 bytes into the keep while the
  first map's textures were read).
- **Each launch:** 67 BSP loads, none failed; 31 pools taken and given
  back; render memory owned at its peak 2,715,648 bytes (2,711,552 in the
  second launches) and 0 at the end; arena
  restored, no failed allocation; main stack peak 105,248 bytes of 131,072.
- **Second launches** place the arena 4 KiB higher, so the pool is 4,096
  bytes smaller (2,707,456) and every pool margin above 4,096 bytes less
  (BSP 6: 1,025,120). BSP 3's ninth base map is then a smaller one
  (1,421,504 bytes resident, 35,767 surfaces textured), and one floor
  pose's CRC differs between the launches (below).
- **Switch cost:** a BSP load (comparison, tables, lightmap pages and base
  maps from the card) takes 111 to 356 ms emulated; the comparison alone
  12 to 91 ms.

## Textures: coverage per BSP

Base maps are `gx-splitalpha-v1` (HWI-008F): 27,807,296 bytes for a10's 9
BSPs. Each BSP load keeps the lightmap pages, then base maps by the surfaces
they cover while they fit the budget and the pool. That is the simple
per-BSP residency the ticket allows; the streaming cache stays HWI-054's.

| BSP | Base maps | Under the 3 MiB budget alone: resident, surfaces | With the pool: resident, surfaces | Pool needed for the budget's set | Short |
|---:|---:|---|---|---:|---:|
| 0 | 12 (2,142,848) | 12, 40,934 / 40,934 (100%) | 10, 40,842 (99.8%) | 3,176,160 | 464,608 |
| 1 | 13 (2,995,040) | 13, 54,800 (100%) | 8, 53,929 (98.4%) | 4,534,368 | 1,822,816 |
| 2 | 14 (3,344,672) | 13, 40,638 / 40,642 (99.99%) | 9, 39,841 (98.0%) | 4,222,592 | 1,511,040 |
| 3 | 14 (3,344,672) | 13, 36,540 / 36,544 (99.99%) | 9, 36,306 (99.3%) | 4,108,704 | 1,397,152 |
| 4 | 12 (2,645,280) | 12, 52,088 (100%) | 8, 49,394 (94.8%) | 4,159,456 | 1,447,904 |
| 5 | 19 (4,153,792) | 16, 56,634 / 56,648 (99.98%) | 10, 53,055 (93.7%) | 4,522,688 | 1,811,136 |
| 6 | 13 (2,995,040) | 13, 57,532 (100%) | 7, 54,128 (94.1%) | 4,677,376 | **1,965,824** |
| 7 | 17 (5,092,512) | 9, 6,937 / 7,808 (88.8%) | 7, 4,683 (60.0%) | 4,416,000 | 1,704,448 |
| 8 | 8 (1,093,440) | 8, 17,192 (100%) | 8, 17,192 (100%) | 2,186,656 | 0 |

"Surfaces" are the environment surfaces whose material has a base map.
What the frames show agrees: of the visible environment triangles drawn
in the a10 runs, the share drawn with a resident base map is BSP 0 99.0%,
BSP 1 99.8%, BSP 5 97.0%, BSP 7 62.6%, BSP 8 100%. Base maps left out are
logged with their bytes (`RENDER_BASE_NOT_RESIDENT reason=pool`); none is
left out for the budget.

**Shortage, escalated (no capability cut):** to hold every BSP's 3 MiB
budget set, the pool needs up to 1,965,824 bytes more (BSP 6; 1,969,920 at
the +4 KiB placement). Ways to close it, for HWI-054: stream base maps by visibility, mip level first
(a BSP's visible base maps are its textured surfaces' share above); lend
the sound region until sound starts (4,194,304 bytes); or
keep lightmap pages at a lower mip until seen.

## Drawing a10 through its BSP switches

**The script's own switches** (the a10 line, 9,000 ticks at 30 Hz, 60 Hz
and retrace pacing): BSP 0 at tick 1, then 7, 1 at tick 1,307, 8 at tick
3,634 and 0 at tick 6,971 (6,973 retrace-paced), each loaded and drawn in
its first frame. The engine camera is the player unit's (HWI-016B), so the
cinematic shows little: sampled frames (16×12 grid) cover 5 to 8 of 192
samples in BSP 7, 4 to 7 in BSP 1, 192 in BSPs 8 and 0.

**Every other BSP** (`a10sw`, 800 ticks, the script runtime's
`(switch_bsp n)` at ticks 300, 360, ..., 720 into 2, 3, 4, 5, 6, 1, 8, 7):
each BSP is loaded and drawn. The player stands where BSP 7 put him, so
the engine camera sees nothing in BSPs 2, 3, 4 and 6; their pixels are
checked at the floor poses below.

| Per BSP, all a10 frames of a launch | 0 | 1 | 2–4, 6 | 5 | 7 | 8 |
|---|---:|---:|---:|---:|---:|---:|
| Frames | 6,958 | 9,884 | 120 each | 120 | 6,388 | 13,947 |
| Triangles a frame, mean / max | 3,688 / 3,980 | 2,582 / 5,955 | 0 / 0 | 3,675 / 4,211 | 6,119 / 6,357 | 171 / 8,042 |
| CPU µs a frame, mean / max (emulated; GPU not modelled) | 1,527 / 1,649 | 1,405 / 1,982 | 821–1,212 | 1,754 / 1,846 | 885 / 910 | 399 / 1,265 |

## Game state against the i686 host, drawing on

The Wii draws every frame of every run. The host draws nothing and replays
each launch's arena offset and measured retrace frame times.

The plan (`map.txt`) is HWI-016F's 12 lines with `a10sw` extended: Prisoner
at 60 Hz after its warm-up; Prisoner, three cycles of six cadences; the six
refused loads; Prisoner at 30 Hz; `a10bsp` (the damaged BSP refused at the
switch, BSP 7 kept and drawn); `a10sw` (800 ticks, the eight labelled
switches); a10, 9,000 ticks at three cadences after a 30-tick warm-up.

| Launch | Guest | Plan | Run lines equal to the host | Allocation checkpoints | Ticks compared, first divergent | a10 simulation, all three cadences | Host process |
|---|---|---|---|---:|---|---|---|
| Titan 1 | `END result=pass` | 12/12 | 28/28 | 1,794 | 10,710, none | `f752db70` | `0x0`, 1,584.9 s |
| Titan 2 | `END result=pass` | 12/12 | 28/28 | 1,794 | 10,710, none | `03181654` (+4 KiB) | `0x0`, 1,583.8 s |
| Research 1 | `END result=pass` | 12/12 | 28/28 | 1,794 | 10,710, none | `f752db70` | `0x0`, 1,581.9 s |
| Research 2 | `END result=pass` | 12/12 | 28/28 | 1,794 | 10,710, none | `03181654` (+4 KiB) | `0x0`, 1,581.8 s |

- **Rendering writes no game state:** the whole state, every allocation
  and every tick equal the host's, which never draws. Each fixed pose also
  digests the whole game state before and after its draws: equal at all 44
  poses of every launch.
- **a10's placement dependence** predates this ticket: HWI-016F's JSON has
  `03181654` in both second launches too. The host gives the same digest at
  the same offset, so the comparison holds; the HWI-016F record's
  "`f752db70` in every launch" describes the first launches.
- **The door fix** (`device_machines.c`). `machine_update` tested
  `*((byte *)unit + 0xb6)`: on the little-endian Xbox the low byte of the
  object's `damage_flags`, on a big-endian port the high byte. In `a10sw`
  after the switch into BSP 5 at tick 480, a biped stood in a door's
  activation radius; the Wii opened it (device group 67 at 1.0, its
  changed-once bit set) at tick 484 and the host did not, and objects,
  effects and particles followed. Located by dumping the whole state on
  both sides at tick 484: only the device groups differed, one element.
  Drawing played no part (the same divergence with `render=off`). The
  field is now read as a value; little-endian ports test the bit they always
  did. `test_engine_byte_reads.py` rejects any engine read of a structure
  field through a byte pointer at a fixed offset (this was the only one).

## Fixed poses (EFB, 32×24 samples)

Three pose phases after the map runs, each a fresh load at 30 Hz with every
frame drawn: Prisoner (HWI-016B's 5 poses); a10 through its own switches,
the engine camera at ticks 30, 1,500, 4,000 and 7,500 with two labelled
floor poses each; and a10 with the eight labelled switches, the engine
camera at each and two floor poses each. Each pose draws full,
lightmap-only, base-only, base-alpha, a textured mask, and the three cull
modes, then shows the full frame.

**Floor poses** (new, labelled `diagnostic_floor`): a10's BSP bounds are
mostly rock, so HWI-016B's bounds-based overrides stand in solid space and
draw nothing. A floor pose stands 0.6 units above one of the BSP's largest
upward-facing triangles where the collision BSP finds an open leaf, and
looks level toward the BSP's centre; the second is in another cluster
where one exists (BSPs 7 and 8 have one cluster).

Research launch 1, full-frame CRCs:

| BSP | Engine camera | Floor a | Floor b | Covered (a / b) | Textured samples (a / b) |
|---:|---|---|---|---|---|
| 0 | tick 7,500 `dec17d88` | `f4a719d2` | `f9826d3c` | 764 / 768 | 764 / 768 |
| 1 | tick 1,500: empty | `1a7ae0dd` | `696cbdf0` | 768 / 768 | 341 / 768 |
| 2 | tick 330: empty | `ba33d160` | `e677718d` | 768 / 768 | 768 / 764 |
| 3 | tick 390: empty | `3d789977` | `11fd474e` | 768 / 669 | 692 / 597 |
| 4 | tick 450: empty | `4a67c5ca` | `cb5ace11` | 768 / 768 | 394 / 768 |
| 5 | tick 510 `fcd80250` (768 covered) | `f0bd1ae1` | `b0920af0` | 768 / 768 | 281 / 731 |
| 6 | tick 570: empty | `af164b50` | `624c0161` | 768 / 765 | 754 / 765 |
| 7 | tick 30 `5c7e8618` (19 covered) | `58266c2b` | `10c1019b` | 768 / 767 | 0 / 0 |
| 8 | tick 4,000 `a8ba8c54` (768) | `73ba116d` | `77812704` | 768 / 768 | 768 / 768 |

- **Every pose passes, in every launch:** the game state unchanged; under
  `GX_CULL_BACK` no sample shows a back face, under `GX_CULL_FRONT` none a
  front face; the lightmap changes every covered frame; and wherever a
  resident base map shows (the textured mask), the base map changes the
  frame. BSP 7's floor poses see only materials whose base maps did not fit
  the pool: lightmap-only, counted, and so labelled.
- **Within a host** the CRCs repeat between launches, except BSP 3's floor
  b in the second launch (its full, base-only and base-alpha CRCs, and 5
  textured samples), where the 4 KiB smaller pool keeps a different ninth
  base map. **Across hosts** the colour differs (D3D and Vulkan filter
  textures differently); covered, textured and facing counts are equal.
- **Titan launch 1, full CRCs:** BSP 0 `dfd3a98e` `89413df8` `45f8d985`;
  BSP 1 floor `f62d5e8e` `8c44a0eb`; BSP 2 `f2ba37e2` `02054671`; BSP 3
  `8ba67a74` `114b5d59`; BSP 4 `b8879ed4` `b461587a`; BSP 5 `7cae500a`
  `b574b6d5` `b0920af0`; BSP 6 `4c80c6ee` `f48046b5`; BSP 7 `2bdba2bd`
  `58266c2b` `10c1019b`; BSP 8 `4695ec0f` `f595473c` `5e07978b`. BSP 7's
  floor poses and BSP 5's floor b equal Research's: lightmap pages alone,
  or nearly, show there.

## Two cold launches on each host

Stock Dolphin 2609 in a fresh isolated profile (`tools/wii/run_dolphin.py
--runs 2`): Titan on Windows 11 with D3D, Research on macOS 27 with Vulkan.
The second launch of each pair reads the first's persisted result and
places the arena 4 KiB higher.

| Check | Titan 1 | Titan 2 | Research 1 | Research 2 |
|---|---|---|---|---|
| Guest (`END`) | pass, 0 failures | pass, 0 failures | pass, 0 failures | pass, 0 failures |
| Persistence (runner) | passed | passed (read `961a5ec5`) | passed | passed (read `961a5ec5`) |
| Host process | `0x0`, 1,584.9 s | `0x0`, 1,583.8 s | `0x0`, 1,581.9 s | `0x0`, 1,581.8 s |
| OS | no instability events | no instability events | no instability events | no instability events |
| HWI-015A scenario | `c1ad6ae6` | `c1ad6ae6` | `c1ad6ae6` | `c1ad6ae6` |
| Refused loads; damaged BSP | 6/6; 7→7 | 6/6; 7→7 | 6/6; 7→7 | 6/6; 7→7 |
| Geometry checks equal | 67/67 | 67/67 | 67/67 | 67/67 |
| Poses passed | 44/44 | 44/44 | 44/44 | 44/44 |

The guest's elapsed time is 1,580.7 s, against HWI-016F's 1,124.3 s with
drawing off: the three pose phases add 8,547 drawn frames, and the a10
runs take 313.5 s at 30 Hz (304.1 s undrawn).

## Review frames

A poses-only launch on Research with frame dumps (the labelled-switch
phase: 779 frames) gave three review frames, kept privately and not in
Git: BSP 2's floor pose a, BSP 6's floor b and BSP 8's floor a. The guest
finished (`END result=pass`, 27/27 poses), then Dolphin faulted at
shutdown (`0xfffffff5`, signal 11, a Dolphin crash report in the batch
window, no OS instability events). HWI-016B saw the same exit code once on
Research; it is the shutdown-only class ADR-017 defers to HWI-050, recorded
here, not tolerated away. No evidence launch faulted.

## Timings

| Step | Host | Wall time |
|---|---|---|
| Wii engine build (`ninja wii_engine`, 572 units: every change moves the build ID) | Titan | 80 s |
| The other seven CI targets after it | Titan | 5 s |
| Host reference build (WSL Debian clang) | Titan | 80–81 s |
| Host reference and comparison of one evidence launch | Titan | 52 s (66 s for the first) |
| Geometry option and coverage measurement | Titan | under 2 s |
| Evidence plan, one launch | Titan, Dolphin D3D | 1,584.8 s (development launch, build `71dbfc8f`) |
| Two cold launches | Titan, Dolphin D3D | 3,192 s (1,584.9 and 1,583.8 s each), after a 53-minute wait for the lock behind HWI-034 and HWI-033 |
| Two cold launches | Research, Dolphin Vulkan | 3,180 s with transfers (1,581.9 and 1,581.8 s each); load average 3.80 3.25 4.28 before, 2.42 2.28 5.09 after |
| Poses-only check | Research | 116.6 s (379 s with the lock wait and transfers) |

## Not covered

- **Physical Wii.** Untested. GX reads the vertex arrays at their tag-slot
  addresses; all of a10's materials start on 32 bytes
  (`vertex_data_unaligned32=0`), but hardware alignment and cache behaviour
  are unproven.
- **Texture residency** beyond the pool (above): HWI-054.
- **Lighting:** the environment path reads no normal; detail maps, bump,
  specular, sky, objects and the rest stay unsupported and report
  themselves (HWI-016B's list).
- **The menu path to a10** is HWI-033's.

## Cross-port scope

The door fix is in `source/` and runs on every port; little-endian ports
read the same bit as before. Everything else is in `port/wii`.
