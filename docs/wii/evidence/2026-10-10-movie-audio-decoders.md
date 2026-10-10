# Movie and audio decoders: licences, Dolphin benchmarks and the movie transcode (HWI-008D)

This evidence chooses the Wii decoders for the game's five movies and its
sound, measures them in stock Dolphin, checks A/V sync over every movie, and
adds the import-time movie transcode to the content pipeline. Only the
game's own five Bink movies and its Xbox ADPCM sound are in scope; the
disc's demo-launcher videos and WMA/WAV audio are excluded (ADR-019).
Owned bytes, file names, frames, audio and paths stayed outside Git: movies
are "movie 1" to "movie 5", by disc path order.

**Every timing below is a guest-timebase number from Dolphin's CPU timing
model. It is an emulator number and does not establish Wii hardware
performance.** The hardware run is written down in
[the last section](#hardware-run-to-do-later-hwi-040).
[Numeric evidence](2026-10-10-movie-audio-decoders.json) ·
[Third-party notes](../../../port/wii/third_party/README.md) ·
[Benchmark DOL](../../../port/wii/media_bench/README.md) ·
[Pipeline guide](../CONTENT-PIPELINE.md#movies)

## Result

| Decision | Choice | Reason, from the numbers below |
|---|---|---|
| Movie format | MPEG-1 video + MP2 audio in an MPEG program stream | 4.1 to 5.9 times less decode-plus-present time per frame than MJPEG + PCM, depending on quality (13.3 against 72.6 ms on the full movie 3 at q8), and 4.6 times smaller (96 MB against 443 MB for all five) |
| Movie decoder | pl_mpeg (MIT) with the bit-exact HWI-008D speed patch | Frame-for-frame identical output to stock pl_mpeg and to the host build, 59.5 to 61.0 dB from FFmpeg's own MPEG-1 decoder; the patch cuts mean decode time by 13 to 17 % |
| Settings | `-q:v 8`, 640×480, 30000/1001 fps, MP2 192 kbit/s (profile `wii-mpeg1-q8-mp2-v1`) | The only measured setting that plays all five movies with at most 3 dropped frames (movie 5's heaviest passage) in Dolphin; q6 dropped 38 and q4 96 frames of movie 5. Picture quality 39.9 to 43.0 dB Y-PSNR against the Bink source |
| Size for all five movies | 96,053,248 bytes (from 647,097,636 bytes of Bink) | Measured by the pipeline run, not extrapolated |
| Game sound | Keep Xbox ADPCM as stored and decode on the CPU | 1.2 ms per second of mono 22,050 Hz audio, 4.9 ms for stereo 44,100 Hz (0.12 % to 0.49 % of a CPU per voice), bit-exact with an independent decoder. DSP-ADPCM is not possible with ASND or AESND |
| Not chosen | MJPEG + PCM (stb_image) | 0.46 times real time: 57 ms decode plus 15 ms present per frame |

The A/V check passed in Dolphin: over the five movies (543.5 s), the audio
clock at the moment each frame became visible stayed within −11.3 to
+38.8 ms of the frame's time, its median between −1.8 and +11.6 ms, and its
least-squares slope within ±0.73 ms per minute on the four long movies.
Movies 1 to 4 dropped no frame; movie 5 dropped 3 of 481.

## Source and identity

| Item | Value |
|---|---|
| Benchmark DOL | `media_bench.dol`, build `9d4121ec964e140d`, source `3b8fbb51` (clean), SHA-256 `6986cd8b…14711` |
| Wii compiler | devkitPPC GCC 16.1.0, C11, `-O2 -Wall -Wextra -Werror -ffp-contract=off` |
| Host reference compiler | Debian GCC 14.2.0 (WSL), `-std=c11 -O2 -Wall -Wextra -Werror -ffp-contract=off` |
| Dolphin | stock 2609; Titan `Dolphin.exe` SHA-256 `1ed5e3f6…0dac` (D3D), Research `Dolphin` SHA-256 `b3c369bc…3ccf` (Vulkan); fresh isolated profile per batch, stock clock and memory, `--batch --exec --movie` with an authored neutral DTM |
| FFmpeg (host tool only) | the user's 6.0 "essentials" build, `ffmpeg` SHA-256 `e9fd5e71…49db1`, `ffprobe` `139f8420…a951` |
| Image SHA-256 | `43ec0813…0a7258` |
| Hosts | Titan (Windows 11 x86-64) and Research (M5 Max Mac Studio, macOS) |

## 1. Licence review

Each download came from the official upstream repository at a pinned
commit, into its own new empty directory, and was read before it was built.
The vendored files are stored byte for byte with their SHA-256 pinned and
tested (`tools/wii/test_third_party.py`).

| Component | Role | Version and SHA-256 | Licence | Patent status | Obligations for this public fork | Vendored |
|---|---|---|---|---|---|---|
| pl_mpeg | MPEG-1 video, MP2 audio, program-stream demuxer | `c871f2be` (2025-12-30), `3a8cb30c…c2c9` | MIT (SPDX header; the full MIT text with "Copyright (c) 2019 Dominic Szablewski" until upstream `3198122a`) | MPEG-1 video and Layer II: no essential patents remain. Wikipedia's MPEG-1 article states that all declared video patents have expired and that US 5,214,678, cited for Layer II, has expired. The Library of Congress format description lists none known. | Keep the copyright and permission notice (`pl_mpeg/LICENSE`); state the modification (`pl_mpeg_wii.patch`, authored, MIT) | Yes |
| stb_image v2.30 | Baseline JPEG for the MJPEG candidate | `2c980bb5` (2026-08-02), `594c2fe3…00b3` | MIT or Unlicense (public domain), at the user's choice | Huffman baseline JPEG only; the Forgent '672 patent's term ended in 2006 (Wikipedia, JPEG); the arithmetic-coding options are not supported | Keep the notice (MIT option) | Yes (benchmark only; not chosen) |
| Xbox ADPCM decoder | Game sound | Authored in `media_core.c` | The fork's CC0 | Published IMA ADPCM algorithm (1992) | None | Authored |
| ASND (libogc) | Audio output | Official devkitPro `libasnd.a` | BSD 3-clause (Hermes, 2008) | — | Reproduce its notice in a binary release's documentation | Linked from the toolchain |
| FFmpeg 6.0 | Host transcode | The user's build | LGPL/GPL | — | None: not distributed or linked | No |

Not used: the RAD Bink SDK, Nintendo's THP decoder and Nintendo's DSP-ADPCM
(AX) SDK code. All licences above are permissive and compatible with the
GPL and with `DATA-AND-LICENSES.md`. This is engineering due diligence, not
legal advice.

## 2. Benchmark method

`port/wii/media_bench` decodes privately transcoded clips of the owned
movies and HWS1 game sounds from the emulated SD card. Movie 5 (the 16.0 s
movie) and movie 3 (a full 68.1 s movie) were benchmarked for both formats
and three MPEG-1 qualities.

- **video jobs** load the whole file into MEM2 first, so the decode time holds
  no SD I/O. Each frame is decoded, packed into the back external framebuffer
  (Y'CbCr 4:2:2, the format the video interface scans out) and flipped. Decode
  and present are timed separately with the guest timebase.
- **Correctness.** The DOL folds the CRC-32 of every presented framebuffer into
  a sequence CRC, which must equal the host build of the same code. Every job
  in every launch matched, so PPC and x86 output is identical, frame for
  frame, including MP2's floating-point synthesis. The host output was then
  compared with FFmpeg's own decoders (table below).
- **av jobs** play a movie in real time from SD: MP2 or PCM audio streams to an
  ASND voice, which is the master clock, interpolated between ASND's
  1,024-sample mixing ticks with the timebase. Up to five decoded frames wait
  in XFB slots. A frame is flipped when the audio clock is within one field of
  its time and dropped if it is more than one frame late. The post-retrace
  callback stamps the audio clock when each frame becomes visible.
- The MJPEG candidate decodes to RGB (stb_image), so its present step converts
  RGB to Y'CbCr. MPEG-1 already gives Y'CbCr 4:2:0, so its present step only
  interleaves.

## 3. Benchmark results (Dolphin; Titan and Research gave identical guest numbers)

Both hosts and both launches on each host produced the same guest timings,
CRCs and A/V records for build `9d4121ec`; the second launches' logs are
byte-identical between Titan and Research. Dolphin's guest timing is
deterministic. The host only changes wall time. The real-time frame period is
33.37 ms.

| Clip, decoder, setting | Decode mean / p95 / max (ms) | Present (ms) | Real-time factor, mean / p95 | Frames over the period | Worst 1 s window (ms per frame) | Late frames in the five-frame queue model | Sequence CRC = host |
|---|---|---|---|---|---|---|---|
| Movie 5, pl_mpeg, q4 | 11.48 / 34.66 / 43.58 | 3.36 | 2.25 / 0.88 | 48 of 481 | 41.29 | 38 | yes |
| Movie 5, pl_mpeg, q6 | 9.86 / 29.74 / 37.44 | 3.36 | 2.52 / 1.01 | 23 | 35.79 | 0 | yes |
| Movie 5, pl_mpeg, q8 | 8.90 / 26.49 / 33.51 | 3.36 | 2.72 / 1.12 | 8 | 32.35 | 0 | yes |
| Movie 5, stb_image MJPEG q5 | 56.58 / 58.43 / 59.22 | 15.36 | 0.46 / 0.45 | 481 | 74.12 | 473 | yes |
| Movie 3, pl_mpeg, q4 | 14.54 / 29.82 / 39.45 | 3.36 | 1.86 / 1.01 | 100 of 2,042 | 30.49 | 0 | yes |
| Movie 3, pl_mpeg, q6 | 11.56 / 24.71 / 33.65 | 3.36 | 2.24 / 1.19 | 13 | 24.99 | 0 | yes |
| Movie 3, pl_mpeg, q8 | 9.95 / 21.86 / 30.40 | 3.36 | 2.51 / 1.32 | 4 | 21.78 | 0 | yes |
| Movie 3, stb_image MJPEG q5 | 57.21 / 59.24 / 59.65 | 15.36 | 0.46 / 0.45 | 2,042 | 74.82 | 2,034 | yes |

The real-time factor is the frame period over decode-plus-present time. The
queue model replays a job's measured times through the A/V job's five-frame
queue; it leaves out MP2 decode and SD reads, so the A/V runs below are the
binding result.

**Real-time playback of movie 5 (its heaviest passage), q4 / q6 / q8:**
96 / 38 / 3 frames dropped of 481, decode mean 12.6 / 10.7 / 9.6 ms with
audio and SD reads, A/V drift −14.9 to +35.1, −5.5 to +44.6 and −12.7 to
+37.3 ms. That is why the default is q8.

**pl_mpeg speed patch.** The same jobs with stock pl_mpeg (development build
`5e8c2e28`, one launch on Research) decoded in 13.82 / 11.73 / 10.54 ms mean
(q4 / q6 / q8, movie 5) and 16.86 / 13.29 / 11.41 ms (movie 3), against
11.48 / 9.86 / 8.90 and 14.54 / 11.56 / 9.95 ms patched; movie 5 dropped
142 / 83 / 44 frames instead of 96 / 38 / 3. The patch reads up to 25 bits
from one 32-bit window, reads VLC bits inline and copies full-pel prediction
rows with `memcpy`. On the host, its per-frame CRCs equal stock pl_mpeg for
all six benchmark encodes, and in Dolphin both builds gave the same sequence
CRCs. The profile at the time put about half of the host decode time in
macroblock prediction and a third in bit reading; IDCT was small.

**XFB packing.** Assembled 32-bit word stores (build `f3072513`, two launches
on Research) measured 3.79 ms per frame against 3.36 ms for byte stores, so
the final build keeps byte stores.

### Picture quality (host)

PSNR over all frames, from the total squared error per plane; FFmpeg 6.0
decodes the reference and the transcodes.

| Comparison | Movie 5 Y / U / V (dB) | Movie 3 Y / U / V (dB) | Worst frame (dB) |
|---|---|---|---|
| pl_mpeg against FFmpeg's MPEG-1 decoder, same q4 stream | 60.99 / 65.72 / 67.05 (max difference 4) | 59.47 / 66.75 / 67.95 (max 6) | 56.0 / 55.8 |
| stb_image against FFmpeg's JPEG decoder, RGB | 47.35 | 48.20 | 39.0 / 42.9 |
| MPEG-1 q4 against the Bink source | 47.21 / 48.19 / 48.43 | 43.75 / 46.85 / 47.89 | 41.7 / 41.1 |
| MPEG-1 q6 against the Bink source | 44.71 / 46.10 / 46.41 | 41.46 / 45.23 / 46.33 | 39.1 / 38.6 |
| MPEG-1 q8 against the Bink source | 42.98 / 44.70 / 45.08 | 39.92 / 44.15 / 45.26 | 37.4 / 36.9 |
| MJPEG q5 against the Bink source | 47.74 / 48.91 / 49.19 | 44.01 / 47.50 / 48.59 | 43.0 / 41.2 |

The Bink source is itself lossy, so these figures measure distance from it,
not from an original master. The pl_mpeg-to-FFmpeg gap is IDCT rounding.

### Audio (Dolphin)

| Decode | Cost per second of audio | Share of a CPU at real time | Check |
|---|---|---|---|
| MP2 192 kbit/s stereo 44,100 Hz (pl_mpeg, movies 5 and 3) | 35.6 ms | 3.6 % | PCM CRC = host |
| Xbox ADPCM mono 22,050 Hz (two sounds, 29.5 s) | 1.21 to 1.22 ms | 0.12 % | PCM CRC = independent Python decoder |
| Xbox ADPCM stereo 22,050 Hz (two sounds, 12.0 s) | 2.43 ms | 0.24 % | same |
| Xbox ADPCM stereo 44,100 Hz (two sounds, 8.9 s) | 4.88 to 4.89 ms | 0.49 % | same |

The six sounds are the longest and the median permutation of each of the
three formats in the converted generation. At these rates, for example, 32
mono 22,050 Hz voices would cost about 3.9 % of a CPU (an estimate from
these numbers, not a measurement).

**DSP-ADPCM through ASND or AESND: not possible.** The voice formats of
libogc's ASND (`VOICE_MONO_8BIT` … `VOICE_STEREO_16BIT_LE`) and AESND
(`VOICE_MONO8` … `VOICE_STEREO16_UNSIGNED`) are 8- and 16-bit PCM only.
Hardware ADPCM decoding needs a DSP microcode that drives the accelerator
with ADPCM coefficients, as Nintendo's AX does; neither library's microcode
does. The conversion would not be lossless either: IMA and DSP-ADPCM use
different predictors, so it would be a second lossy encode at about the same
size (4 bits per sample). With CPU decoding at 0.12 to 0.49 % per voice,
there is no reason to pursue it.

### Memory

| Item | Bytes |
|---|---|
| pl_mpeg, video only (three 640×480 4:2:0 frames plus buffers) | 1,669,244 decoder heap peak |
| pl_mpeg, A/V from SD (video, audio, 128 KiB file buffer, demuxed packets) | 2,193,532 to 2,717,820 at q8 over the five movies; 3,766,396 for movie 5 at q4 |
| stb_image, one frame | 1,402,831 (the A/V job also holds a 1 MiB compressed-frame buffer) |
| XFB | 614,400 per slot; the benchmark allocates six (3,686,400) for the A/V queue, two for the decode jobs |
| Heap in use around the jobs | 3,855,520 at start and end of each launch: the decoders free everything |

## 4. A/V sync over every movie (q8, Research, two cold launches)

Both launches gave identical records.

| Movie | Frames | Presented / dropped | Drift min / median / max (ms) | First → last drift (ms) | Drift slope (ms per minute) | Frames later than one field |
|---|---|---|---|---|---|---|
| 1 (151.4 s) | 4,538 | 4,538 / 0 | −2.0 / −1.8 / +31.5 | 9.9 → 14.7 | −0.09 | 8 |
| 2 (132.9 s) | 3,982 | 3,982 / 0 | −5.3 / +11.4 / +28.2 | 11.5 → 11.4 | +0.02 | 6 |
| 3 (68.1 s) | 2,042 | 2,042 / 0 | −5.1 / +11.6 / +28.3 | 15.4 → 11.6 | +0.72 | 12 |
| 4 (175.1 s) | 5,247 | 5,247 / 0 | −6.4 / +10.3 / +10.8 | 10.8 → 10.3 | +0.18 | 0 |
| 5 (16.0 s) | 481 | 478 / 3 | −11.3 / +5.4 / +38.8 | 5.4 → 5.4 | −6.49 (drops, not clock drift) | 32 |

Decode time with audio and SD reads averaged 7.8 to 12.6 ms per frame
(maximum 36.2 ms). Dolphin's emulated SD read each whole movie in under a
third of a second of guest time, which says nothing about real cards.

Drift is the audio clock when the frame became visible minus the frame's
presentation time; positive means the picture is behind the sound. No audio
underrun occurred. At the last frame the audio clock was within 30 ms of
each movie's length and within 20 ms of the guest time elapsed since the
voice started, so no audio was lost or repeated. The VI and audio clocks
kept a constant offset (slope within ±0.06 ms per minute on movies 1 to 4).

One offset is not in these figures. In all five program streams the MP2
stream's first timestamp is 10.9 ms earlier than the video's (FFmpeg's
muxing). pl_mpeg measures both streams from the video's start, so playback
starts the first audio sample with the first frame: the sound is 10.9 ms
later against the picture than the stream intends, which partly offsets the
positive drift above. The engine's player can remove it by applying the
streams' start-time difference.

## 5. Pipeline integration (Titan)

`tools/wii/movie_convert.py` adds only new files and uses HWI-008B's
`content_publish.py` unchanged; `content_convert.py`, `gx_texture.py` and
the other `content_*.py` modules were not touched.

| Run | Result | Wall time |
|---|---|---|
| Fresh root A, default q8 | exit 0, generation `ebdc2e9b6fb39b33`, 5 movies, 96,053,248 bytes, 20 demo-launcher files listed as excluded | 44.5 s |
| Fresh root B, default q8 | exit 0, the same generation and manifest SHA-256 (`ebdc2e9b…166b`) | 45.6 s |
| Root A again | exit 0, existing generation verified and reused | 74.0 s |
| Fresh root, q4 | exit 0, 177,260,544 bytes | 67 s |
| q6, stopped before `CURRENT` | exit 4, `CURRENT` unchanged, no staging left; the renamed generation (121,927,680 bytes) is harmless | 58 s |
| Simulated disk full after 20 MB | exit 4, `CURRENT` unchanged, no staging left | 23 s |
| Stopped before validation | exit 4, `CURRENT` unchanged, no staging left | 57 s |

The published q8 generation re-verified after every fault. The pipeline's
movies 3 and 5 are byte-identical to the benchmark clips, and the A/V runs
played the pipeline's own five outputs. Every output kept its Bink header's
frame count (4,538, 3,982, 2,042, 5,247, 481), the 640×480 size and 44,100 Hz
stereo audio. The image was re-hashed after each run and was unchanged.

| Movie | Bink bytes | q8 bytes | q6 bytes | q4 bytes | MJPEG + PCM bytes |
|---|---|---|---|---|---|
| 1 | 191,746,528 | 32,012,288 | 40,509,440 | — | 138,473,322 |
| 2 | 164,906,308 | 24,387,584 | 30,566,400 | — | 102,869,024 |
| 3 | 79,102,884 | 11,835,392 | 14,755,840 | 20,996,096 | 51,024,624 |
| 4 | 200,614,260 | 24,989,696 | 32,718,848 | — | 140,945,113 |
| 5 | 10,727,656 | 2,828,288 | 3,377,152 | 4,454,400 | 9,325,696 |
| **All five** | **647,097,636** | **96,053,248** | **121,927,680** | **177,260,544** | **442,637,779** |

HWI-008B's estimates (155 MB for MPEG-1 q4, 316 MB for MJPEG) were
extrapolated from movie 5; the measured totals are 177 MB and 443 MB.

The excluded files are the demo launcher's executable, its three videos and
its 16 media files (WMA, WAV, images, fonts and two unidentified files).
None is read past its header or transcoded.

## 6. Launch records

| Batch (build `9d4121ec`) | Host | Guest | Persistence | Host lifecycle | OS stability | Wall time |
|---|---|---|---|---|---|---|
| Benchmark, 19 jobs, 2 cold launches | Titan | passed, run counter 1 → 2 | passed | exit 0 both (416.1 s, 413.1 s) | no instability events observed | 888.0 s |
| Benchmark, 19 jobs, 2 cold launches | Research | passed, run counter 1 → 2 | passed | exit 0 both (409.9 s each) | no instability events observed | 832.3 s |
| A/V, five movies, 2 cold launches | Research | passed, run counter 1 → 2 | passed | exit 0 both (544.9 s each) | no instability events observed | 1,102.2 s |

Persistence means the run counter and log were written, read back after
each cold launch and advanced by one, the staged inputs were unchanged, and
every job's per-frame record file was present. Both hosts ran the guest at
the real-time limit (`EmulationSpeed 1.0`), so a launch took the same wall
time on each.

Stopped batches, kept in the private record and not counted above:

- Titan benchmark of build `f3072513` (word-store packing), stopped by the
  agent after the measurement above showed the slower packing.
- Research A/V and a second Titan benchmark of build `9d4121ec`, stopped by
  the agent because the driver listed later runs' record files as read-backs,
  which fails an earlier run's read-back. The driver was fixed and both were
  rerun. Stopping the Research batch left its Dolphin running outside the
  host lock for under a minute; the agent then ended it.

## 7. Timings (wall clock)

| Step | Host | Time |
|---|---|---|
| Full Wii build, all eight targets, fresh configure | Titan | 109 s |
| `ninja wii_media_bench` after a change | Titan | about 5 s |
| Benchmark clips: three encodes of movie 5 / movie 3 / movie 4 | Titan | 2.0 s / 10.9 s / 49 s |
| Movie transcode, all five, q8 | Titan | 44.5 s and 45.6 s (39.6 s and 41.4 s inside the tool) |
| Benchmark launch (19 jobs) | Titan / Research | 413.1 and 416.1 s / 409.9 s |
| A/V launch (543.5 s of movies) | Research | 544.9 s |
| Host PSNR comparisons, movie 5 | Titan | 25 s |

## Hardware run to do later (HWI-040)

None of the timings above is a hardware number. When Devin's Wii is ready,
run the same DOL on the console, with the same inputs:

1. Build `ninja wii_media_bench` at the commit that carries this evidence
   and check that `build/wii/media_bench.dol` has SHA-256 `6986cd8b…14711`
   (full value in the JSON), or record the new build ID.
2. Publish the movies with `tools/wii/movie_convert.py` (default profile),
   and again with `--profile wii-mpeg1-q6-mp2-v1` and
   `--profile wii-mpeg1-q4-mp2-v1` into separate roots. Movies 3 and 5 of
   each generation are the benchmark clips.
3. Compute the expected CRCs with the host build in
   `port/wii/media_bench/README.md`: `media_bench_host mpeg1 <file> crcs.bin`
   prints `sequence_crc` and `audio_crc`; `media_bench_host adpcm <file>`
   prints the PCM CRC.
4. On a FAT32 SD card, copy the clips to `sd:/hwm/...`, and the six HWS1
   sounds (the longest and the median permutation of each format in the
   `content_convert.py` generation) to `sd:/hwm/adpcm/`. Write
   `sd:/halo-wii-media/jobs.txt` with the Dolphin batches' job lines:
   `video mpeg1` for q4, q6 and q8 of movies 5 and 3, `mp2` for movies 5
   and 3, the six `adpcm` jobs and `av mpeg1` for q4, q6 and q8 of movie 5;
   then, as a second job set, `av mpeg1` for the five q8 movies.
5. Copy the DOL to `apps/halo-wii-media/boot.dol` and start it from the
   Homebrew Channel. It returns to the loader when the jobs end. Do two cold
   launches per job set, powering the console off between them. Watch and
   listen to one A/V launch: lips and effects must match the picture and the
   sound must not break up.
6. Copy the card's `halo-wii-media` directory to the host and run
   `python -B tools/wii/media_bench_summary.py --sd-copy <copy>`. Compare
   with this evidence: every `result=pass` and the same sequence CRCs first,
   then decode times, dropped frames and drift.
7. Keep q8 unless the hardware plays all five movies at q6 or q4 with no
   dropped frame and a clear margin. If even q8 drops frames, the next steps
   are more decoder work (a table-driven VLC reader, paired-single IDCT and
   prediction) before lowering the resolution.

Record the console model, system menu version, the IOS the loader uses, the
SD card make and size, and the wall time of each launch.

## Not established

- Any hardware number: decode time, dropped frames, drift and SD read speed
  on a Wii (HWI-040). Dolphin does not model cache misses or bus contention.
- The engine's movie player: integration with the 30 Hz simulation, pausing,
  skipping, and handing the XFB back to the renderer.
- Presenting through GX (for overlays or scaling) instead of the XFB.
- The engine's sound mixer at its real voice count; the ADPCM numbers are per
  voice.
- FAT behaviour of the published movie tree on real SD media.

HWI-008 stays open; HWI-008D's own hardware numbers are deferred to HWI-040.
