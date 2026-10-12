# Playing the game's sound on the Wii (HWI-074)

The Wii engine build now plays the game's sound. The engine's own sound
manager and its Xbox sound platform run unchanged. What they call of the
Xbox's DirectSound is now a software mixer on the Wii
([port/wii/sound](../../../port/wii/sound/README.md)). The mixer reads the
game's Xbox ADPCM from the converter's HWS1 containers, which are staged as a
per-map sound pack. Its output goes through one ASND voice. Turning the sound
on changes no game state, in the simulation or the whole state, on Prisoner
or a10.
[Numeric evidence](2026-10-11-wii-sound-output.json)

**Result:**

- **Game state unchanged by sound.** With the sound on, Prisoner's
  simulation stays `127656ef` / `961a5ec5` and its 60 Hz whole state stays
  `7eb53576`. a10's simulation stays `f752db70` at 30 Hz, 60 Hz and the
  measured retrace. Every measured run's whole state equals the sound-off
  run's on the i686 host (Prisoner at 6 cadences, a10 at 3). On the Wii,
  every run equals the i686 host at every tick, with and without sound, in
  both cold launches on Titan and on Research. Three closings and five
  withheld reactions were needed (below).
- **Decoded PCM is bit-exact.** The Wii's decoder (the same C, built for the
  host) equals the host reference decoder (`media_formats.decode_xbox_adpcm`)
  on every permutation of both packs: 7,939 permutations, 234,975,232
  frames. In each launch, every permutation the Wii loaded and decoded (441
  and 440) equals the host decoder.
- **The mix is bit-exact.** Every run's mix CRC (each 16-bit sample mixed)
  and its stream-event CRC equal the i686 host's. That is 11 runs per
  launch, 4 launches. Titan's and Research's guests mixed identical audio.
- **What the Wii's DSP played** (Dolphin's audio dump, against the host's
  mix):
  - in Prisoner's retrace-paced run, 67 of 77 tenth-second windows are the
    host's samples at ASND's volume, 89.1 % of all samples;
  - in a10's (60 s of music, dialogue and ambience), 511 of 599 windows,
    94.6 % of samples.

  The other windows hold an underrun's silence or a drop.
- **Sounds follow game events** (Prisoner's scripted input, the engine's
  sound classes):
  - footsteps from tick 32 (walking from 30);
  - weapon fire from tick 91 (trigger 90–110 and 210–230);
  - the reload at tick 282 (input 280);
  - landings and impacts from tick 61 (jump at 60);
  - a10's music from tick 2, ambience from 1,512 and scripted dialogue from
    1,042.
- **Voices:**
  - Prisoner: at most 14 (all 3D mono), 4.3 on average;
  - a10's opening: at most 19 (1 2D, 12 3D, 2 stereo 22 kHz, 8 stereo 44 kHz),
    6.0 on average.

  The engine's own limits decide: of 40 weapon-fire starts, 36 were stopped
  by the engine's per-definition instance limit. No platform limit applies:
  81 streams are mixed on the CPU.
- **Mixer cost** (Dolphin's guest clock, not hardware):
  - 30.6 ms per audio second (3.1 %) in Prisoner;
  - 66.7 ms (6.7 %) in a10's opening;
  - at most 5.9 ms in one frame at 30 Hz.
- **Underruns:**
  - Prisoner paced by the retrace keeps up: 0 frames dropped, 10 silent
    buffers in 5 runs of at most 4 (85 ms), out of 468 played;
  - a10's opening does not keep real time in this harness: its 9,000 ticks
    take 350 s of emulated time unpaced, so its retrace run underruns
    (2,060 buffers) and drops its bursts.

## Build

| Item | Value |
|---|---|
| Source | Clean `33dc7902` (merges `wii` at `bae56603`), HWI-027's build-ID rule |
| Build ID | `c3824938242060e7`, `source_dirty=false` |
| `engine.dol` | 4,232,480 bytes, SHA-256 `8f6aec1f03a3988141e69853581a049919b79f0655896c8d2fc15e918482f02e` |
| `engine.elf` | 38,089,476 bytes, SHA-256 `6610a54ff1b9403bbc7c8a1f8ea4a3021bf127b866d98f13ed786143e52417cd` |
| Toolchain | devkitPPC GCC 16.1.0; libogc's `libasnd` (BSD-3: its notice ships with releases, HWI-064) |
| Host reference | i686, Debian clang 19.1.7, the same engine and platform units (the ASND output is engine.dol's only) |
| Artifacts | `package_artifacts.py` stage and inspect `--require-clean`: 33 files, 0 problems, 0 findings |
| Maps | HWI-016C's stagings (HWIMAP02), unchanged |
| Sound packs | `sound_stage.py`, from converter generation `6e7b87372c08e3fa`: Prisoner 930 permutations (13,377,856 bytes), a10 7,009 (153,168,676 bytes); all Xbox ADPCM |
| MEM1 image | +135,776 bytes against `bae56603`. Of these, code is 40,776 and BSS 88,557: the ASND library 25,816, the output ring 49,152 and 4,096 of silence. MEM1 after the image: 2,929,440 bytes |
| Heap | +222,760 bytes when the sound starts (the engine's sound arrays, 81 streams, the pack's offsets), flat across cycles |

## Design

- **The engine decides; the platform mixes.** These run unchanged:
  - `sound_manager.c`: channels, priorities, instance limits, fades and
    gains;
  - `sound_dsound_xbox.c`: 81 DirectSound streams, packets and 3D
    parameters.

  `dsound_wii.c` implements the DirectSound calls the engine makes, as
  `port/linux/src/dsound_sdl.c` does on Linux, but in integers. Pitch is
  linear resampling in 16.16. Gains have 14 fractional bits and ramp over a
  256-frame chunk. 3D voices get DirectSound's inverse distance rolloff,
  an equal-power pan, and the I3DL2 direct path's obstruction and occlusion
  as a one-pole low pass. 2D voices use their mix bins. Gains are worked
  out in floats once a call and then rounded, so two compilers and machines
  mix alike. Doppler, cones and the I3DL2 reverb are not modelled.
- **On the game's thread and clock.** The engine calls `DirectSoundDoWork`
  every frame. It mixes as many frames as the engine's clock has moved
  (48 a millisecond), then completes the packets it has played, and the
  engine's callback refills them. What the game sees of the sound follows
  its own clock.
- **ADPCM a block at a time.** A stream reads its packet where the engine's
  sound cache holds it, and decodes one 64-frame block as the mixer reaches
  it (HWI-022's decoder).
- **Bounded buffers.**
  - Each stream holds the engine's 4 packets in 6 slots, plus one decoded
    block.
  - The mix goes out in 256-frame chunks.
  - ASND takes one 1,024-frame buffer per mixing tick, from a ring of 12
    (256 ms). The voice starts at 3 buffers (64 ms of latency).
  - An empty ring plays a silent buffer, which is counted as an underrun. A
    full ring drops the newest frames, which are counted. The game never
    waits on the sound.

  The first Dolphin run used 512-frame buffers and played at half rate:
  ASND asks for one buffer per tick.
- **Data through the cache layer.** `cache_files_wii.c` opens the map's
  `<name>.wsnd` beside `<name>.wmap`. A sample read outside the staged
  segments (the engine's `cache_file_read` of a permutation) is served from
  the pack. The pack checks the HWS1 header against its entry, and the
  samples against the map's CRC. Without a pack, reads fail as before.
- **The map run's `sound` plan line** starts the engine's own
  `sound_initialize` before the line it precedes. Each run frame then
  renders the sound as `main_loop` does, on the frame clock moved by the
  frame's time. The listener follows the player's deterministic camera while
  the sound manager runs; the simulation keeps the run's observer.

## Keeping the game state unchanged

Enabling the engine's sound reaches the game state in the original engine.
Each path was found on the i686 host by tracing allocation digests per tick
and dumping the whole state at the first differing tick. Each is now
closed, on the Wii build and its host reference only:

| Path | What changed with sound on | Closing |
|---|---|---|
| Random numbers | A sound that a tick starts draws a skip chance, a pitch and a permutation from the tick's local stream, even when unheard (Prisoner's effects then diverged) | The sound manager draws from a stream of its own (`HALO_SOUND_RANDOM_STREAM`, `random_math.c`), seeded and reset like the others |
| Collision marks | The sound's obstruction tests and the listener camera's test mark visited objects (`magic_number`, `global_object_marker`) | The marker and every object's mark are put back when the sound's code returns |
| The sound's tag fields | Promotion counters, played permutations and cache places in the sound tags | The map run's tag-slot digest reads them as zeros while the sound runs |
| Unit speech | A scripted line that plays notifies its unit, which speaks (a10 from the first line) | Withheld (`couplings=presentation`) |
| Mouth | The line's mouth data sets the unit's mouth aperture once a frame. This is cadence-dependent: 30 Hz and 60 Hz diverged at a10 tick 7,119 | Withheld |
| Class fades | Scripted class fades move gains in the game state, once a frame | Faded on a copy outside the game state |
| Looping sound lifetime | An unattached looping sound is freed when its stop has played, which moves later datum indices that objects keep | The game gets the no-sound answer, and the stop still plays |
| Kept indices | The first-person weapon and unit dialogue keep a started sound's index | They keep `NONE` |

`couplings=original` keeps the Xbox's behaviour for HWI-040 to weigh.
With it, a10's simulation is not cadence-invariant (the mouth) and differs
from `f752db70` (unit speech).

## Dolphin launches

Two cold launches per host from `33dc7902`, `--allow-concurrent` (other lanes
running), `--dump-audio`. Plan:
1. Prisoner at 60 Hz;
2. Prisoner at six cadences, without sound;
3. the `sound` line;
4. the same two Prisoner lines, then a10 for 9,000 ticks at 30 Hz, 60 Hz
   and the measured retrace.

| Host | Launch | Guest | Persistence | Host exit | OS | Elapsed (shared host) |
|---|---|---|---|---|---|---|
| Research (M5 Max, macOS, Vulkan, Dolphin 2609) | 1 | pass, 7/7 expectations | passed | 0 | no instability events | 1,215.9 s |
| Research | 2 | fail: 6/7 (below) | passed | 0 | no instability events | 1,215.6 s |
| Titan (Windows 11, D3D) | 1 | pass, 7/7 | passed | 0 | no instability events | 1,227.3 s |
| Titan | 2 | fail: 6/7 (below) | passed | 0 | no instability events | 1,232.1 s |

- **Launch 2's failure** is the a10 expectation `expect_sim=f752db70`,
  which this plan added. The second launch places the arena 4,096 bytes
  lower, and a10's canonical simulation digest is `03181654` there. The
  i686 host gives `03181654` too, at that placement. The host also gives it
  without sound, on `bae56603` and on `6d898c7c`, before any HWI-074 change:
  "object globals" and "players globals" differ canonically from tick 1.
  This is a pre-existing placement dependence for HWI-026's harness. Every
  launch-2 run still equals its host reference at every tick.
- **Wii against host** (the host replays each launch's placement and
  retrace frame times): without sound, 468 allocations and 600 ticks equal;
  with sound, 702 allocations, 9,000 a10 ticks and 600 Prisoner ticks
  equal. Every sound run's decisions, mix CRC and event CRC are equal, with
  no decode mismatches. This holds in all four launches.
- **Titan against Research:** identical guest digests and mix CRCs in every
  run, the retrace runs included.
- **Host events:** Research's batch window shows 7 Dolphin application events
  (crash reports) that the runner does not attribute to this lane, which
  shared the host with other lanes' Dolphins. This lane's Dolphins exited
  with 0. Titan shows none.

## Timings

| Step | Host | Seconds |
|---|---|---|
| Engine build, clean | Titan | 66 to 103 |
| i686 host reference build | Titan (WSL) | 179 |
| Sound packs (Prisoner, a10) | Titan | 0.7, 3.7 |
| Full-pack decode check (both packs, 12 processes) | Titan | 27.8 |
| Evidence batch, two launches | Research (shared, 3 lanes) | 2,448 wall |
| Evidence batch, two launches | Titan (shared, 2 lanes) | 2,554 wall |
| Host reference, one launch's plan | Titan (WSL) | 86 to 149 |

Shared-host timings are labelled (`host_shared=true`), and must not be
compared with exclusive runs.

## Not done

- **Mixer cost** on hardware, and lowering it. a10's 6.7 % of guest time
  is a lot for the Wii; DSP voices for simple 2D sounds or paired-single
  mixing are options for HWI-040.
- **The I3DL2 reverb**, cones and Doppler.
- **The game's reactions to sound** (unit speech, lip-sync, class fades):
  a decision for HWI-040.
- **Moving the 52 KB output ring out of MEM1.**
- **Boot-mode sound** (`boot.txt sound=1`) is wired (`wii_sound_wanted`)
  but untested. Movies remain their own ticket.
- **A10 under the harness:** it does not keep real time at the retrace,
  so its audio underruns there.
- **Physical Wii:** untested.

Private (never committed): the sound packs, Dolphin's audio dumps, the
host's mix captures and every engine log stay under the operator's private
run directory.
