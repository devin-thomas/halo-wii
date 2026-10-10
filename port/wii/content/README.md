# Wii content loaders (HWI-008C, HWI-008E)

Runtime loaders for the converted content containers, and a self-test DOL
that proves them against converter output in Dolphin. The repository holds
no game data: the SD card supplies the files.

| Unit | Container | What it does |
|---|---|---|
| `hwt_texture.c` | HWT1 texture | Validates the header (magic, version, GX format against the `gx-baseline-v1` source mapping, dimensions, faces, levels, image count, reserved bytes, exact data length) and yields every image (face, level, slice) with its offset. `hwt_decode_texel` decodes one texel by the GX rules, CMPR's 5/8 + 3/8 blend included. |
| `hra_animation.c` | HRA1 recorded animation | Validates the header, the unit-control table of its version, every event (type, delta kind, delta, payload widths, unused words zero, one end event, last) and the delta sum. `hra_encode_xbox` rebuilds the Xbox stream; `hra_verify_source` checks it against the recorded source tag. |
| `hws_sound.c` | HWS1 sound | Validates codec, rate, channels, reserved bytes, frames and payload length. `hws_decode_adpcm` decodes Xbox ADPCM blocks to PCM16, reading each block header explicitly little-endian. |
| `hwm_model.c` | HWM1 model geometry | Parts (vertex type, strip type, counts, geometry/part order, contiguous ranges, every index below its part's vertex count). Decodes to GX indexed arrays: `hwm_vertex` (56 bytes) with F32 position, F32 normal/binormal/tangent expanded from 11:11:10 by the engine's formula, s16 texcoord, node bytes and weight as stored; u16 indices. |
| `hwl_lightmap.c` | HWL1 BSP lightmap geometry | BSP record against the sections, lightmap material ranges, every material (vertex types, counts, ranges, surfaces, local surface indices). Decodes materials, `hwl_vertex` (56 bytes: F32 position, expanded NBT, F32 texcoord), `hwl_lightmap_vertex` (16 bytes: expanded incident radiosity, s16 lightmap texcoord) and u16 surfaces. |
| `hwc_collision.c` | HWC1 collision | Per-BSP ranges and maxima, and every index the engine follows: node planes and children (node, NONE, leaf by sign bit), leaf references, 2D references and nodes (node or surface by sign bit), surface planes (flip bit) and edges, edge vertices, edges and surfaces, vertex edges. Decodes to the engine's record layouts. |
| `hma_graph.c` | HMA1 model animations | Index, pad, node count, frame info type, frame size against the node flags, the four data ranges, sizes, and each compressed block's header, node headers and arrays tiling it exactly. Decodes to the Xbox byte layout in native order; compressed blocks stay compressed. |
| `hwf_font.c` | HWF1 font | Font record against the sections, tables tiling the indices, every index and every glyph's pixels. Decodes records and keeps 8-bit glyph coverage verbatim; `hwf_character_for` is the engine's lookup. |
| `hus_strings.c` | HUS1 unicode text | String count and lengths, ranges tiling the code units. Decodes to native u16 code units. |
| `content_sections.c` | - | The shared sectioned-container parser, record formats compiled to field offsets, big-endian to native record decoding, the little-endian digest serialization, the 11:11:10 expansion and the owned arena. |
| `content_common.c` | - | Error names, big-endian field readers, loading a whole file into owned 32-byte-aligned memory, SHA-256. |
| `content_check.c` | - | The case list and per-file checks shared by the DOL and the host build. |

Every field is assembled from bytes with shifts; nothing depends on host
byte order, struct layout or bit-fields (ADR-018). Each decoded struct is
checked against its record format with static assertions. The sources build
for the Wii with the strict flags (C11, `-Wall -Wextra -Werror`) and for a
host (`host_check.c`, used by `tools/wii/test_content_loaders.py`).

## Decoded forms and digests

The six HWI-008E loaders (`hwm_load`, `hwl_load`, `hwc_load`, `hma_load`,
`hwf_load`, `hus_load`) validate the whole file first, then decode it into
one owned 32-byte aligned arena; the input file can be released at once.
Each has a `*_release` and a `*_digest`: the SHA-256 of every decoded field
written little-endian in the documented order, which
`tools/wii/content_decoded.py` computes independently on the host. For model
animations, unicode string lists and fonts the decoded data also rebuilds
the converter's source hash of the original Xbox bytes.

Renderer and engine decisions (details in each header):

- **Packed vectors:** the 11:11:10 normal, binormal, tangent and incident
  radiosity are expanded at load to F32 with the engine's own
  `uncompress_int32_to_real_vector3d`, bit for bit. GX has no 11:11:10 format;
  S16 or S8 normals would not be exact. It costs 36 bytes per vertex instead
  of 12.
- **Texture coordinates:** model and lightmap texcoords stay s16 (GX S16);
  the engine's `(2s + 1) / 65535` is affine and goes in the texture matrix.
  Environment texcoords are already F32.
- **Indices:** 16-bit (GX_INDEX16). Every part and material has at most
  65,535 vertices, and GX has no 32-bit indices. Model parts are
  precompiled strips with degenerate joins (GX_TRIANGLESTRIP); BSP surfaces
  are triangle lists local to their material.
- **Skinning:** GX takes one matrix index per vertex, so the two-node
  weighting is the renderer's (HWI-032). Node bytes and weights stay as
  stored: on the retail data 94% of vertices have node bytes that are not
  multiples of 3, so the engine's `/3` decode does not apply to them.
- **Compressed animations:** stay compressed and are decoded at playback by
  the engine's keyframe code, as on the Xbox. Some 4-byte arrays inside a
  block sit at 2-byte offsets; the playback reader must load them
  alignment-safely.
- **Glyphs:** 8-bit coverage stays linear; the text renderer fills a GX
  I8/I4 cache texture with the glyphs it draws.

## Self-test DOL

```sh
python configure.py --wii --wii-devkitpro <devkitPro root>
ninja wii_content_loaders            # build/wii/content_loaders.dol
python -B tools/wii/content_loader_cases.py --root <content output root> --out <new private dir>
python -B tools/wii/run_dolphin.py --dolphin <Dolphin> --dol build/wii/content_loaders.dol \
  --build-info build/wii/content_loaders-build-info.json --out <new dir> --runs 2 --efb-access \
  --input-script tools/wii/inputs/content-loaders.json --scenario content_loaders \
  --sd-image <blank FAT32 image> --stage <LOCAL=sd:/path for each line of stage.txt>
# every model, lightmap, collision, model animation, font and text output through the host build:
python -B tools/wii/content_loader_cases.py --sweep --root <content output root> --out <new private dir>
```

`content_loader_cases.py` samples every texture format and type, sound
class and recorded-animation version of a published generation, and for
each sectioned container the smallest, median and largest file plus the
extremes (largest collision model and structure BSP, compressed animations,
the font with the most glyphs, the longest string list, a HUD text). It
writes the host digests of their decoded contents and derives malformed
variants: truncations, bad magics, versions, enums, counts, lengths,
reserved bytes, events, deltas, value widths and a changed source tag for
the HWI-008C formats; header, section-table and padding defects plus
per-format range, count and enum defects for each HWI-008E format; and
three wrong-format controls (a file given to another format's loader).

The DOL reads `sd:/halo-wii-content/cases.txt` and runs every case twice:

- loads the file into owned memory and checks its SHA-256;
- valid files: decodes all contents and compares the digest with the host's;
  uploads every GX-loadable texture image, and each level of a 2D chain
  through one mipmapped texture object with its LOD pinned, and reads every
  texel of a 32 x 32 crop back from the EFB (colour, then alpha through
  `GX_CC_TEXA`), which must equal the CPU decode exactly (CMPR interpolated
  texels may differ by 8);
- decoded kinds: the file is released before the digest, the heap is
  measured with the file and decoded form held (`heap_peak`) and with the
  decoded form alone (`heap_resident`);
- a control draws image 0 as a different GX format of the same size, which
  must read back differently for at least one texture per cycle;
- malformed files must reject with the stated reason.

Both cycles must agree, and the heap must end where it started.
`sd:/halo-wii-content/loaders.log` holds one `CASE` line per case, `CYCLE`,
`TIMING`, `MEMORY` (the largest decoded asset per kind), `SUMMARY` and
`END` lines; `runs.txt` counts launches.

## Limits

Emulator only; physical Wii and real SD/FAT behaviour are untested. Timings
are Dolphin's emulated time base. GX has no cube or volume textures: their
faces and slices are checked as 2D images. Textures above 1,024 texels on a
side are decoded on the CPU but not uploaded. Sound decoding is checked
against digests, not played. Geometry is decoded and checked, not drawn;
skinning, the texture matrices and compressed-animation playback belong to
their consumers.
