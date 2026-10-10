# Host content pipeline (HWI-008 baseline)

This pipeline turns your own Halo data into Wii-ready, versioned derived
files on your computer. It runs on the host with Python 3.11 or later and
needs no Wii, Dolphin or devkitPro. It never changes the source files and
refuses to write inside the checkout. The source, the outputs and the private
reports stay outside Git and are never published.

What it covers today: an inventory of every map, tag group, bitmap, sound,
recorded animation, font and string tag, and every movie file; GX conversion
of every bitmap; explicit conversion of recorded animations; and sound
permutations in a Wii container. Models, collision, shaders, scripts, fonts,
movies and the runtime loaders are not converted yet (see
[open items](#open-items)).

## Inputs

The tools read the private staging that the earlier intake produced:

- a staging directory holding `staging.json` (each map's file name, size and
  SHA-256) and `maps/<name>.map`. Every map is hashed while it is read and
  must match `staging.json`.
- optionally, for movies and other disc files, the XISO image and the JSON
  report from `tools/wii/xiso_inventory.py`. The image hash must match both.

Supported source: Xbox cache version 5, build `01.10.12.2276`, the build
recorded for the supplied NTSC dump. Other builds are rejected by the
header checks or need their own validation first.

## Commands

Run the commands from the repository root. Every output path must be new
and outside the checkout.

```powershell
# 1. Inventory: private full report and sanitized public summary
python -B tools/wii/content_inventory.py --staging <staging-dir> `
  --image <image.iso> --image-inventory <xiso-inventory.json> `
  --private <private-dir>/inventory-private.json --public <private-dir>/inventory-public.json

# 2. Convert and publish a generation (all maps and categories by default)
python -B tools/wii/content_convert.py --staging <staging-dir> `
  --output-root <private-dir>/content --record <private-dir>/run-1.json
#    optional: --maps <map> <map>   --categories textures animations sounds
#              --verify-per-kind N (items re-decoded per format/type/map; default 2)

# 3. Movie decode probe with your own FFmpeg build (host only)
python -B tools/wii/probe_movies.py --image <image.iso> --image-inventory <xiso-inventory.json> `
  --ffmpeg-bin <dir-with-ffmpeg-and-ffprobe> --output <private-dir>/movie-probe.json `
  --transcode-sample <private-dir>/movie-transcode

# Tests (authored fixtures only; no game data)
python -B -m unittest discover -s tools/wii -p "test_*.py"
```

`content_convert.py` exit status:

| Status | Meaning | Published generation |
|---|---|---|
| 0 | Published (or an identical generation already existed) | New or reused |
| 1 | Invalid input, failed check or bad arguments | Unchanged |
| 3 | Not enough free space for the planned outputs | Unchanged, nothing written |
| 4 | I/O failure such as a full disk, or an interruption | Unchanged |

The run record (`--record`) holds timings, verification counts, the fault
settings and the source re-hash results. It is never overwritten.

## Output layout

```text
<output-root>/CURRENT                      "<generation> <manifest sha256>"
<output-root>/generations/<generation>/manifest.json
<output-root>/generations/<generation>/textures/<map>/<tag>-<bitmap>.hwt
<output-root>/generations/<generation>/animations/<map>/<index>.hra
<output-root>/generations/<generation>/sounds/<map>/<tag>-<range>-<perm>.hws
```

The generation id is the first 16 hex digits of the manifest's SHA-256. The
manifest is canonical JSON with sorted keys and holds no timestamps or host
paths. It records:

- the converter name and version, and the SHA-256 of each converter module
  (line endings normalized), so a code change gives a new generation;
- the full profile and its hash;
- the source chain: the image hash and the `staging.json` hash;
- per source map: stored and inflated hashes, sizes, build and category;
- per output: path, size, SHA-256, kind, source identity (map, tag ordinal,
  index, format and dimensions, source byte hash) and, for textures, the GX
  format, image count and whether the dimensions fit GX.

The same input and profile always give byte-identical manifest and output
files, so the generation id is the same.

### Publication and failure behaviour

1. Leftover `.staging-*` directories from killed runs are deleted, since they
   are never trusted.
2. For each map, every item is planned and validated before any of that map's
   output is written: counts, addresses, spans, layouts, codecs and streams.
   One invalid item fails the whole run.
3. Free space is checked against the planned output size plus 16 MiB.
4. Outputs are written into a new `.staging-<random>` directory with
   exclusive creation and an fsync per file. Then `manifest.json` is written.
5. The tree is re-read: the exact file set, sizes, hashes and each
   container's header.
6. The staging directory is renamed to `generations/<id>`. If that id already
   exists it is verified and reused.
7. `CURRENT` is replaced through a temporary file and `os.replace`.

If a run fails, is interrupted or hits a full disk at any step, `CURRENT` and
every earlier generation are left as they were. A generation that was renamed
but never made current is harmless, and the next identical run reuses it.
`content_publish.read_current()` re-verifies the whole published tree before
use.

Relative output paths follow FAT32-safe rules:

- lower-case `[a-z0-9._-]` components, at most 64 characters each and at most
  6 levels deep;
- no `.` or `..`, no absolute or drive paths, and no reserved device names;
- unique when compared case-insensitively;
- every file below 4 GiB.

Publication on a Windows NTFS host was tested. FAT behaviour on real SD
media, including rename atomicity and directory size, still has to be
qualified on the console.

## Formats (all big-endian)

**HWT1 texture.** A 64-byte header:

| Field | Type |
|---|---|
| magic `HWT1` | 4 bytes |
| version | u16 |
| GX format (libogc numbering) | u16 |
| width, height, depth | u16 each |
| faces, levels, images | u16 each |
| source format, source type | u16 each |
| data offset (64), data bytes | u32 each |

GX images follow in face, level, slice order. Each image is one GX-tiled 2D
level and starts 32-byte aligned. Cube maps are six chains in Xbox hardware
face order. Volume textures are per-level depth slices. GX has neither, so
the renderer decides how to use them.

**HRA1 recorded animation.** A 32-byte header, then:

- the unit-control fields, each as a 32-bit word: signed values
  sign-extended, floats as their raw IEEE-754 bits;
- six 16-bit animation-state values, sign-extended (current codec only);
- one 16-byte record per event: type (u8), delta kind (u8; `0xFF` for the
  v1 codec), delta (u16) and three payload words;
- any bytes after the end event, copied verbatim.

Every field is decoded with explicit little-endian reads and shift/mask
header decoding, never host struct layout or bit-fields (ADR-018).
Rebuilding the Xbox stream from the record must reproduce the source bytes
exactly, or the item fails.

**HWS1 sound.** A 32-byte header: magic, version, codec (1 = Xbox ADPCM,
2 = PCM16 big-endian), sample rate (u32), channels, frames and payload bytes.
The payload follows. Xbox ADPCM payloads are kept verbatim: 36-byte blocks
per channel holding 64 samples, with a little-endian header word that the
runtime decoder must read explicitly. Uncompressed PCM16 is byte-swapped to
big-endian.

## Profile `gx-baseline-v1`

| Halo format | GX format | Fidelity |
|---|---|---|
| dxt1 | CMPR | Block data is lossless. GX blends the two middle colours at 5/8+3/8 instead of 2/3+1/3, a difference of at most \|c0−c1\|/24+1 per channel. The colour of transparent texels is unspecified on GX. |
| dxt3, dxt5 | RGBA8 | Exact against the reference decode, at 4× the source bytes |
| p8_bump | RGBA8 | Exact through the engine's `global_vector_palette`, at 4× the source bytes |
| a8r8g8b8, x8r8g8b8 | RGBA8 | Exact |
| r5g6b5 | RGB565 | Exact |
| a1r5g5b5 | RGB5A3 | Opaque texels are exact. Transparent texels keep alpha 0, with 4-bit colour. |
| a4r4g4b4 | RGB5A3 | Colour is exact. Alpha is rounded to the nearest of 8 levels (maximum error 17/255). |
| a8y8 | IA8 | Exact |
| a8, y8 | IA8 | Exact; the missing channel is 255 |
| ay8 | I8 | Exact |

Dimensions are never changed. A texture larger than 1,024 texels on a side
(GX's limit) is converted unchanged and flagged
`loadable_dimensions: false`, and the renderer must decide how to handle it.
Every mip level the Xbox hardware texture holds is converted. Compressed
chains stop at the level whose larger dimension reaches 4, as on the Xbox.

**Verification.** During conversion, `--verify-per-kind` items of each
format and type per map are decoded again:

- every texel of every image is read back by an independent GX decoder;
- each texel is compared with a reference decode of the Xbox source;
- the run fails if any texel is outside the tolerance in the table above.

For Xbox ADPCM sounds, the first 64 blocks of the sampled permutations are
decoded.

## Open items

- **Content not converted yet:** models and geometry beyond the HWI-008A
  section, collision, model animations, shaders, scripts, BSP lightmaps,
  font glyphs and unicode strings (UTF-16LE that must be decoded
  explicitly), and movies.
- **Runtime:** no runtime loader exists for HWT1, HRA1 or HWS1.
- **Residency and storage budgets:** HWI-007 owns these. DXT3/DXT5 to RGBA8
  quadruples those textures; a CMPR colour plane plus an I4/I8 alpha plane
  is a possible smaller profile.
- **Cube maps, volumes and oversize textures:** their handling on GX is a
  renderer decision.
- **Bitmap pixel offsets:** the January engine source
  (`source/cache/xbox_texture_cache.c`) adds the bitmap group's pixel-data
  file offset to each bitmap's offset. On the retail maps that sum makes
  spans overlap, while each bitmap's own offset tiles the pixel region
  without overlap. The converter uses the bitmap's own offset. A Wii texture
  loader must not copy that addition.
- **Movies and audio:** decoder choices are open. The plan and measurements
  are in the [evidence](evidence/2026-10-10-content-pipeline.md).
