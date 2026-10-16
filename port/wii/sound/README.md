# Wii sound (HWI-074)

The game's own sound on the Wii. The engine's sound manager
(`source/sound/sound_manager.c`, `game_sound.c`) and its Xbox platform
(`source/sound/sound_dsound_xbox.c`) run unchanged: they decide what plays,
on which of the engine's channels, with which gains, pitches, positions,
obstruction and occlusion, exactly as on the Xbox. What they call of the
Xbox's DirectSound is implemented here.

| File | Kind | Role |
|---|---|---|
| [`dsound_wii.c`](dsound_wii.c) | platform unit (engine.dol and the i686 host reference) | DirectSound's streams on an integer software mixer: Xbox ADPCM decoded a 64-frame block at a time, linear resampling, 2D mix bins, DirectSound's 3D distance rolloff and equal-power pan, the I3DL2 direct path's obstruction/occlusion low pass, clipping |
| [`xbox_adpcm.c`](xbox_adpcm.c) | platform unit | HWI-022's bit-exact Xbox ADPCM decoder, per block |
| [`sound_pack.c`](sound_pack.c) | platform unit | a map's samples on the card (`d:\maps\<name>.wsnd`), served to the engine's sound cache through the cache layer (`port/wii/engine/cache_files_wii.c`) |
| [`sound_hooks.c`](sound_hooks.c) | engine unit | the map run's sound, its listener, its report lines, and the guards that keep the sound out of the game state |
| [`sound_out_asnd.c`](sound_out_asnd.c) | libogc, engine.dol only | the mix out through one ASND voice |
| [`sound_check.c`](sound_check.c) | host test program | the pack reader and decoder against the host reference (`tools/wii/test_sound.py`) |
| [`fade_check.c`](fade_check.c) | host test program | (HWI-078) the engine's class fades on the ticks against the Xbox's frame, at six cadences (`tools/wii/test_sound.py`) |

## How it runs

- **On the game's thread and clock.** The engine calls `DirectSoundDoWork`
  every frame. It mixes the frames that the engine's clock
  (`system_milliseconds`, 48 frames a millisecond) says have passed, then
  completes the packets it has played, which calls the engine back to refill
  them. What the engine sees of the sound therefore follows only its own
  clock. Under the map run's frame clock, the Wii and the i686 host mix the
  same frames and complete the same packets at the same frames.
- **Bounded buffers.** A stream holds at most the engine's 4 packets (6
  slots) and decodes one 64-frame ADPCM block at a time. The mix goes out in
  256-frame chunks. The output ring holds 12 buffers of 1024 frames (256 ms),
  each one of ASND's mixing ticks. The voice starts once 3 buffers are full
  (about 64 ms of latency).
- **Underruns.** If the voice asks for a buffer that the mixer has not
  filled, it plays a silent one, which is counted. The game never waits on
  the sound. If the mixer writes into a full ring, the newest frames are
  dropped and counted.
- **No ASND voice limit.** The engine asks for up to 73 virtual channels on
  81 streams (its own preferences: 10, 51, 10 and 10 streams of four types).
  ASND has 16 voices, so everything is mixed on the CPU into one voice, and
  the engine's own channel limits and priorities are the only limits.
- **Not modelled.** As in the Linux port: Doppler and cones. The Xbox's I3DL2
  reverb is not modelled either; the Linux port has one, and HWI-040 decides
  whether the Wii can afford it.

## Sound changes no game state

Turning the engine's sound on reaches the game state in several ways. Each
one is closed, so with the sound on the whole game state is the same as
without it:

1. **Random numbers.** A sound that a tick starts draws a skip chance, a
   pitch and a permutation from the tick's local random stream, whether or
   not anything hears it. With `HALO_SOUND_RANDOM_STREAM` (the Wii build and
   its host reference only), the sound manager draws from a stream of its own
   (`source/math/random_math.c`).
2. **Collision marks.** The sound's collision tests (each source's
   obstruction, the listener's camera) mark the objects they visit
   (`magic_number`, from `global_object_marker`). `sound_hooks.c` puts the
   marker and every object's mark back when the sound's code returns.
3. **The sound's own tag fields.** Promotion, played permutations and
   cache places live in the sound tags. The map run's tag-slot digest reads
   them as zeros while the sound runs.
4. **The game's reactions to its sounds** (`couplings=presentation`, the
   default). The Xbox's game did five things because a sound played:
   - a unit is notified that its scripted line plays, so it speaks;
   - its mouth follows the line's mouth data once a frame (on the Wii,
     frames are not ticks);
   - scripted class fades move the class gains in the game state;
   - an unattached looping sound's datum is freed only when its stop has
     played out;
   - two callers keep a started sound's index.

   **Fades (HWI-078, ADR-022): restored, on the ticks.** The engine moves
   the class gains in the game state one tick after each game tick
   (`sound_tick_update`, from `game_time_update`), where the Xbox's frame
   after the tick did, instead of once a render frame on the frame clock.
   This holds on every port and with either `couplings`. At 30 Hz, one
   tick a frame, the gains equal the Xbox's on every tick; at any cadence
   they follow the ticks alone. Without the sound manager (sound off) they
   do not move, as before. `SOUND_RUN` counts `fade_ticks` (ticks a gain
   moved on) and `fade_class_steps`. A run with a scripted fade therefore
   has new digests: the gains are game state (`sound classes`).
   [`fade_check.c`](fade_check.c) checks the engine's `sound_classes.c` at
   the six cadences against the Xbox's frame (`tools/wii/test_sound.py`).
   Measured on the i686 host (2026-10-16), Prisoner, 300 ticks, a script at
   tick 60 fading `ambient` to 0 over 90 ticks and `weapon` to 0.2 over 45:
   `fade_ticks=90 fade_class_steps=585` and simulation `e5a2822b` (chain
   `0b48c174`) at all six cadences. HWI-074's `presentation` left the game
   state's gains unmoved: `dd5856ee`, the same as without sound. At 30 Hz,
   `couplings=original` before this change (fades once a frame) and after
   it are equal on every tick: whole state chain `c87fd4a2`, simulation
   chain `0b48c174`, and the same `mix_crc` and `events_crc`. Without sound,
   and with sound but no fade, nothing changed (Prisoner `127656ef` /
   `961a5ec5`). a10's own script changes a class gain (`fade_ticks=2` in
   9000 ticks): its simulation chain at 30, 60 and 20 Hz is now `d82e5da9`
   (was `3d27851e`); the final state `d509456c` is unchanged.

   For the other four, `presentation` means the game sees what it sees
   without sound. `couplings=original` runs the Xbox's behaviour. With it,
   a10 is no longer cadence-invariant (measured: the mouth). Moving them to
   the ticks is the rest of HWI-078. With `presentation`, scripted lines'
   mouths do not move.

The game's own sound bookkeeping (`game_sound_update`: the background sound
and environment of the camera's cluster, which looping sounds can be heard)
keeps the run's observer. Its choices are game state.

## The map run's `sound` line

```text
sound [listener=player|observer] [verify=0|1] [events=0|1] [couplings=presentation|original]
```

This line goes in `map.txt` (`port/wii/engine/engine_map_run.h`). It starts
the engine's sound once, before the line that follows it; the lines above it
run without sound. Each run's frame then
renders it. `listener=player` (the default) gives the sound's code the
engine's deterministic camera of the player's unit. The run's observer, which
the simulation reads, never moves. `verify=1` logs each permutation's decoded
PCM CRC as it loads (`SOUND_DECODE`). Each run logs `SOUND_RUN`,
`SOUND_KIND`, `SOUND_CLASS`, its first `SOUND_EVENT` lines and, on the Wii,
`SOUND_OUTPUT`. `tools/wii/compare_sound_runs.py` checks them against the
host reference and the host decoder.

## Staging

`tools/wii/sound_stage.py --map <owned map> --generation <converter generation> --output <private dir>`
writes `<map>.wsnd` beside the map's `.wmap`. It checks everything before it
writes, and publishes atomically. The format is described in
`sound_pack.c`'s comment. The pack is owned data: it is never committed.

ASND is libogc's `libasnd` (BSD-3). Its notice ships with releases (HWI-064).
