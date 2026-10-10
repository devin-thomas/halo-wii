# Movie and audio decoder benchmark (HWI-008D)

`ninja wii_media_bench` builds `build/wii/media_bench.dol`. It decodes movie
clips and game sounds staged on the SD card with the decoders chosen for the
port, presents every video frame, and records guest-timebase timings. The
repository holds no game data: you make the clips from your own disc with
`tools/wii/movie_convert.py` (or any FFmpeg transcode in the formats below)
and stage them yourself.

| File | Role |
|---|---|
| `main.c` | Wii harness: SD jobs, XFB presentation, ASND audio, timing, logs |
| `media_core.c/.h` | Authored code shared with the host build: counting allocator, CRC-32, XFB packing, HMJ1/HWS1 headers, Xbox ADPCM decoder |
| `media_decoders.c` | The vendored decoders' implementations ([third_party](../third_party/README.md)) and wrappers |
| `host_main.c` | Host reference build of the same decode code; writes per-frame CRCs |

## Jobs

`sd:/halo-wii-media/jobs.txt`, one job per line:

```text
video mpeg1 <name> <sd path to .mpg> <expected sequence CRC-32, hex>
video mjpeg <name> <sd path to .hmj> <expected sequence CRC-32, hex>
mp2 <name> <sd path to .mpg> <expected audio CRC-32, hex>
adpcm <name> <sd path to .hws> <expected PCM CRC-32, hex>
av mpeg1 <name> <sd path to .mpg> - <frame limit, 0 = all>
av mjpeg <name> <sd path to .hmj> <sd path to .pcm> <frame limit, 0 = all>
```

- **video** loads the whole file into MEM2 first, so the decode time holds no
  SD I/O. Each frame is decoded, packed into the back XFB (Y'CbCr 4:2:2, the
  VI's own format) and flipped without waiting for a retrace. Decode and
  present times are measured separately. The CRC of every presented XFB is
  folded into a sequence CRC, which must equal the host reference.
- **mp2** decodes only the MP2 stream; **adpcm** decodes one HWS1 sound
  container (`tools/wii/content_convert.py`). Both compare a CRC of the
  big-endian PCM with the host reference.
- **av** plays the movie in real time from SD. Audio streams to an ASND
  voice and is the master clock; the clock is interpolated between ASND's
  1,024-sample mixing ticks with the timebase. Up to five decoded frames wait
  in XFB slots; a frame is flipped when the audio clock is within one field of
  its time and dropped if it is more than one frame late. The post-retrace
  callback stamps the audio clock when each frame becomes visible, so drift is
  measured at display time.

`bench.log` gets a `BEGIN` line, one line per job and an `END` line per
launch. `runs.txt` counts launches. `<name>-<run>.bin` holds per-frame
big-endian records: `crc, decode_us, present_us, decoder_heap` for video
jobs and `frame, time_us, dropped, drift_us, vi_minus_audio_us, decode_us`
for av jobs. `tools/wii/media_bench_summary.py` checks and summarises them,
from a `run_dolphin.py` output directory or from a copy of the SD card's
`halo-wii-media` directory.

## HMJ1 (benchmark-only MJPEG container)

A 32-byte big-endian header (`HMJ1`, frames, fps numerator and denominator,
width and height as u16, audio rate, audio channels, a zero word), then
frames+1 u32 offsets from the start of the file, then the JPEG frames. The
audio is a separate raw big-endian PCM16 file. MJPEG was not chosen, so no
converter produces HMJ1.

## Host reference

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -ffp-contract=off \
  port/wii/media_bench/host_main.c port/wii/media_bench/media_core.c \
  port/wii/media_bench/media_decoders.c -lm -o media_bench_host
media_bench_host mpeg1 movie.mpg crcs.bin [planes.i420]
media_bench_host mjpeg movie.hmj crcs.bin [frames.rgb]
media_bench_host adpcm sound.hws
```

`-ffp-contract=off` matters: MP2 synthesis uses floats, and the Wii build
also disables contraction, so the PCM is bit-identical across the two.

## Limits

- Dolphin timings come from Dolphin's CPU timing model. They are emulator
  numbers and do not establish hardware performance.
- Only the full-screen XFB path is measured. Presenting through GX (for
  overlays or scaling) would add a texture upload and an EFB copy.
