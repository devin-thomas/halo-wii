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
| [`sound_hooks.c`](sound_hooks.c) | engine unit | the map run's sound, its listener, its report lines, and the three guards that keep sound out of the simulation |
| [`sound_out_asnd.c`](sound_out_asnd.c) | libogc, engine.dol only | the mix out through one ASND voice |
| [`sound_check.c`](sound_check.c) | host test program | the pack reader and decoder against the host reference (`tools/wii/test_sound.py`) |

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
  256-frame chunks. The output ring holds 16 buffers of 512 frames (170 ms).
  The voice starts once 4 buffers are full (about 43 ms of latency).
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

## Sound changes no simulation state

Turning the engine's sound on reaches the game state in three ways, and each
is closed:

1. **Random numbers.** A sound that a tick starts draws a skip chance, a
   pitch and a permutation from the tick's local random stream, whether or
   not anything hears it. With `HALO_SOUND_RANDOM_STREAM` (the Wii build and
   its host reference only), the sound manager draws from a stream of its own
   (`source/math/random_math.c`).
2. **Collision marks.** A source's obstruction is a collision test, and each
   such test marks the objects it visits (`magic_number`, from
   `global_object_marker`). `sound_hooks.c` restores the marker and every
   object's mark after the sound's tests.
3. **The sound's own fields.** The sound keeps fields in the sound tags:
   promotion, played permutations and cache places. The map run's tag-slot
   digest reads them as zeros while the sound runs.

What remains is the sound's own state, which the whole-state digest still
holds and the simulation digest leaves out (HWI-016F's classification). This
covers the object looping sounds, the game sound globals, the first-person
weapon's current sound, and the scenario's sound environment.

## The map run's `sound` line

```text
sound [listener=player|observer] [verify=0|1] [events=0|1]
```

This line goes first in `map.txt` (`port/wii/engine/engine_map_run.h`). It
starts the engine's sound once, before the first map. Each run's frame then
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
