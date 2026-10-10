# Host content pipeline (HWI-008)

This pipeline turns your own Halo data into Wii-ready, versioned derived
files on your computer. It runs on the host with Python 3.11 or later and
needs no Wii, Dolphin or devkitPro. It never changes the source files and
refuses to write inside the checkout. The source, the outputs and the private
reports stay outside Git and are never published.

What it covers today:

- an inventory of every map, tag group, bitmap, sound, recorded animation,
  font and string tag, and every movie file;
- GX conversion of every bitmap, explicit conversion of recorded animations
  and sound permutations in a Wii container (HWI-008B);
- model render geometry, structure-BSP lightmap geometry, collision BSPs,
  model animation frame data, fonts and unicode text, each in a big-endian
  sectioned container (HWI-008C);
- a census of shaders, whose parameters the engine reads in place;
- Wii runtime loaders for the texture, recorded-animation and sound
  containers, proven in Dolphin ([Wii loaders](#wii-runtime-loaders));
- the game's five movies, transcoded to MPEG-1 video and MP2 audio for the
  vendored pl_mpeg decoder (HWI-008D, [movies](#movies)).

Scripts and the remaining tag fields are not converted yet (see
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
#    optional: --maps <map> <map>
#              --categories textures animations sounds models lightmaps collision
#                           model_animations fonts strings shaders
#              --verify-per-kind N (items re-decoded per format/type/map; default 2)

# 3. Movie decode probe with your own FFmpeg build (host only)
python -B tools/wii/probe_movies.py --image <image.iso> --image-inventory <xiso-inventory.json> `
  --ffmpeg-bin <dir-with-ffmpeg-and-ffprobe> --output <private-dir>/movie-probe.json `
  --transcode-sample <private-dir>/movie-transcode

# 4. Movies: transcode the five game movies and publish them under their own root
python -B tools/wii/movie_convert.py --image <image.iso> --image-inventory <xiso-inventory.json> `
  --ffmpeg-bin <dir-with-ffmpeg-and-ffprobe> --output-root <private-dir>/movies --record <private-dir>/movies-1.json
#    optional: --profile wii-mpeg1-q8-mp2-v1 (default) | wii-mpeg1-q6-mp2-v1 | wii-mpeg1-q4-mp2-v1

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
<output-root>/generations/<generation>/models/<map>/<tag>.hwm
<output-root>/generations/<generation>/lightmaps/<map>/<bsp tag>.hwl
<output-root>/generations/<generation>/collision/<map>/<tag>.hwc
<output-root>/generations/<generation>/model_animations/<map>/<tag>.hma
<output-root>/generations/<generation>/fonts/<map>/<tag>.hwf
<output-root>/generations/<generation>/strings/<map>/<tag>.hus
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
  format, image count and whether the dimensions fit GX;
- `in_place`: per map, the shader tags counted and checked by group.

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

**Sectioned containers (HWI-008C).** HWM1, HWL1, HWC1, HMA1, HWF1 and
HUS1 share one layout (`tools/wii/be_records.py`):

| Part | Contents |
|---|---|
| Header, 32 bytes | magic, version 1, header size 32, section count, flags 0, total file bytes, 16 zero bytes |
| Section table | per section, in ascending id order: id, record bytes, record count, offset, bytes |
| Sections | each at the next 32-byte boundary, zero-padded; the file ends on a 32-byte boundary |

Every record is decoded from the Xbox bytes with an explicit little-endian
field format and re-encoded big-endian with the same format; floats keep
their IEEE-754 bits. Each conversion rebuilds the exact source bytes from
its output or fails. A reader rejects a missing or reordered section, a
wrong record size, count, offset or length, and non-zero padding.

| Container | One per | Sections (record bytes) |
|---|---|---|
| HWM1 | model tag | parts (56), Xbox compressed model vertices (32: position, packed normal/binormal/tangent, s16 texcoords, node indices, weight), u16 strip or list indices |
| HWL1 | scenario structure BSP | BSP (16), lightmaps (12), materials (188, with their render lighting), compressed environment vertices (32), lightmap vertices (8), surfaces (6, indices local to the material) |
| HWC1 | collision model tag, and each structure BSP | BSP ranges (68), then the eight collision arrays: 3D nodes (12), planes (16), leaves (8), 2D references (8), 2D nodes (20), surfaces (12), edges (24), vertices (16) |
| HMA1 | animation graph tag | animations (80), frame info f32 (4), defaults, frames and compressed blocks (bytes, per-field big-endian) |
| HWF1 | font tag | font (24), character tables (8), character indices (2), characters (20), glyph pixels (1, verbatim) |
| HUS1 | unicode string list or HUD message text tag | strings (8), UTF-16 code units (2) |

The field lists are in the module docstrings (`content_geometry.py`,
`content_animation.py`, `content_text.py`). Vertices stay in their Xbox
compressed forms; expanding packed normals and choosing GX vertex formats is
a renderer decision (HWI-032). Compressed model animations keep every array
at its original offset, and the arrays must cover the block exactly.

**Read in place (no conversion).** Shader parameters are typed tag fields
(enums, flags, colours, floats, references, blocks). No shader group has a
tag-data or file-offset field in the upstream validator schema, and the
Xbox pixel programs live in the executable, not the maps. The engine reads
them from the relocated tag cache (ADR-015), which still needs its scalar
fields byte-ordered for the PowerPC (see open items). The converter checks
every shader root lies in tag data and records the counts. 8-bit string
lists (`str#`) are bytes and need no byte-order change.

## Wii runtime loaders

`port/wii/content` holds C loaders for HWT1, HRA1 and HWS1 and a self-test
DOL (`ninja wii_content_loaders`; [README](../../port/wii/content/README.md)).
They validate every header field, count, length and reserved byte, load
files from SD into owned 32-byte-aligned memory and decode explicitly:
texels by the GX rules, recorded animations back to their Xbox stream,
Xbox ADPCM to PCM16. `tools/wii/content_loader_cases.py` stages a
deterministic sample with host digests and malformed variants, and
`run_dolphin.py --scenario content_loaders` checks a run. The
[evidence](evidence/2026-10-10-content-loaders.md) has the Dolphin results.

## Movies

`movie_convert.py` (HWI-008D) reads the five game movies from the image at
the byte ranges the image inventory lists, through FFmpeg's subfile protocol,
so nothing is extracted. It transcodes each with your own FFmpeg build and
publishes one generation with the same staging, validation, rename and
`CURRENT` steps as above. Give it its own output root, because a root has one
`CURRENT` generation.

```text
<movie-root>/generations/<generation>/movies/game-movie-<n>.mpg   n = 1..5, by disc path
```

| Profile | Video | Audio | Five movies |
|---|---|---|---|
| `wii-mpeg1-q8-mp2-v1` (default) | MPEG-1, 640×480, `-q:v 8`, 30000/1001 fps | MP2 192 kbit/s, source rate and channels | 96,053,248 bytes |
| `wii-mpeg1-q6-mp2-v1` | as above, `-q:v 6` | as above | 121,927,680 bytes |
| `wii-mpeg1-q4-mp2-v1` | as above, `-q:v 4` | as above | 177,260,544 bytes |

The container is an MPEG-1 program stream, which the Wii decodes with the
vendored pl_mpeg ([third-party notes](../../port/wii/third_party/README.md)).
The default is the setting whose heaviest measured passage still decodes near
real time in Dolphin. The [decoder evidence](evidence/2026-10-10-movie-audio-decoders.md)
has the reasons. Re-run with another profile once hardware measurements
show the headroom.

- **Checks before publication.** FFmpeg must exit 0 with no messages at its
  `error` level. ffprobe must count exactly the frames in the Bink header,
  one MPEG-1 video stream at the source size, and one MP2 stream at the
  source rate and channel count. Every output must start with an MPEG pack
  header. Any failure fails the run and nothing is published.
- **Determinism.** FFmpeg runs single-threaded with bit-exact flags and no
  metadata. The manifest records the FFmpeg and ffprobe SHA-256 and version
  line, the profile and its hash, this module's hash, the image and image
  inventory hashes, each source's byte-range hash, header facts and output
  hash, with no timestamps or host paths. The same image, FFmpeg binary and
  profile give the same generation.
- **Exclusions.** The disc's demo-launcher files (its executable, videos,
  WMA and WAV audio, images and fonts) are out of scope (ADR-019). The
  manifest lists them as excluded, by kind and ordinal; they are never
  transcoded or copied.
- Exit status as for `content_convert.py` (0, 1, 3, 4). The image is
  re-hashed after the run and must be unchanged.

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

- **Tag fields in place:** shaders, model nodes and regions, BSP clusters,
  scenario and object tags are read in place, but their scalar fields are
  still Xbox little-endian. The upstream validator schema types only blocks,
  references, data, enums and indices, so a complete byte-order pass needs
  full field definitions (HWI-015B loads tags; HWI-008 owns the content).
- **Content not converted yet:** scripts. The demo launcher's files are
  out of scope (ADR-019) and are listed as excluded, never converted.
- **Runtime:** loaders exist for HWT1, HRA1 and HWS1 only. HWM1, HWL1,
  HWC1, HMA1, HWF1 and HUS1 have explicit Python decoders and checks, but no
  Wii loader yet; the engine's consumers (HWI-015B, HWI-016, HWI-032) will
  read them.
- **Compressed vertex expansion:** packed 11:11:10 normals and the s16
  texture coordinates stay as stored; GX vertex formats are a renderer
  decision.
- **Residency and storage budgets:** HWI-007 owns these. DXT3/DXT5 to RGBA8
  quadruples those textures; a CMPR colour plane plus an I4/I8 alpha plane
  is a possible smaller profile.
- **Cube maps, volumes and oversize textures:** their handling on GX is a
  renderer decision.
- **Bitmap pixel offsets:** the January engine source
  (`source/cache/xbox_texture_cache.c`) adds the bitmap group's pixel-data
  file offset to each bitmap's offset. On the retail maps that sum makes
  spans overlap, while each bitmap's own offset tiles the pixel region
  without overlap. The converter uses the bitmap's own offset, and so does
  the Wii texture loader, which reads only the converted file.
- **Movies and audio:** decoders are chosen and measured in Dolphin
  ([evidence](evidence/2026-10-10-movie-audio-decoders.md)): MPEG-1/MP2
  through pl_mpeg for movies, and the game's Xbox ADPCM kept as stored and
  decoded on the CPU. Still open: the engine's movie player, and every
  hardware timing (HWI-040).
