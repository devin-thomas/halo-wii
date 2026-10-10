# Wii content loaders (HWI-008C)

Runtime loaders for the converted content containers, and a self-test DOL
that proves them against converter output in Dolphin. The repository holds
no game data: the SD card supplies the files.

| Unit | Container | What it does |
|---|---|---|
| `hwt_texture.c` | HWT1 texture | Validates the header (magic, version, GX format against the `gx-baseline-v1` source mapping, dimensions, faces, levels, image count, reserved bytes, exact data length) and yields every image (face, level, slice) with its offset. `hwt_decode_texel` decodes one texel by the GX rules, CMPR's 5/8 + 3/8 blend included. |
| `hra_animation.c` | HRA1 recorded animation | Validates the header, the unit-control table of its version, every event (type, delta kind, delta, payload widths, unused words zero, one end event, last) and the delta sum. `hra_encode_xbox` rebuilds the Xbox stream; `hra_verify_source` checks it against the recorded source tag. |
| `hws_sound.c` | HWS1 sound | Validates codec, rate, channels, reserved bytes, frames and payload length. `hws_decode_adpcm` decodes Xbox ADPCM blocks to PCM16, reading each block header explicitly little-endian. |
| `content_common.c` | - | Error names, big-endian field readers, loading a whole file into owned 32-byte-aligned memory, SHA-256. |
| `content_check.c` | - | The case list and per-file checks shared by the DOL and the host build. |

Every field is assembled from bytes with shifts; nothing depends on host
byte order, struct layout or bit-fields (ADR-018). The sources build for the
Wii with the strict flags (C11, `-Wall -Wextra -Werror`) and for a host
(`host_check.c`, used by `tools/wii/test_content_loaders.py`).

## Self-test DOL

```sh
python configure.py --wii --wii-devkitpro <devkitPro root>
ninja wii_content_loaders            # build/wii/content_loaders.dol
python -B tools/wii/content_loader_cases.py --root <content output root> --out <new private dir>
python -B tools/wii/run_dolphin.py --dolphin <Dolphin> --dol build/wii/content_loaders.dol \
  --build-info build/wii/content_loaders-build-info.json --out <new dir> --runs 2 --efb-access \
  --input-script tools/wii/inputs/content-loaders.json --scenario content_loaders \
  --sd-image <blank FAT32 image> --stage <LOCAL=sd:/path for each line of stage.txt>
```

`content_loader_cases.py` samples every texture format and type, sound
class and recorded-animation version of a published generation, writes the
host digests of their decoded contents and derives 38 malformed variants
(truncations, bad magics, versions, enums, counts, lengths, reserved bytes,
events, deltas, value widths and a changed source tag).

The DOL reads `sd:/halo-wii-content/cases.txt` and runs every case twice:

- loads the file into owned memory and checks its SHA-256;
- valid files: decodes all contents and compares the digest with the host's;
  uploads every GX-loadable texture image, and each level of a 2D chain
  through one mipmapped texture object with its LOD pinned, and reads every
  texel of a 32 x 32 crop back from the EFB (colour, then alpha through
  `GX_CC_TEXA`), which must equal the CPU decode exactly (CMPR interpolated
  texels may differ by 8);
- a control draws image 0 as a different GX format of the same size, which
  must read back differently for at least one texture per cycle;
- malformed files must reject with the stated reason.

Both cycles must agree, and the heap must end where it started.
`sd:/halo-wii-content/loaders.log` holds one `CASE` line per case, `CYCLE`,
`TIMING`, `SUMMARY` and `END` lines; `runs.txt` counts launches.

## Limits

Emulator only; physical Wii and real SD/FAT behaviour are untested. Timings
are Dolphin's emulated time base. GX has no cube or volume textures: their
faces and slices are checked as 2D images. Textures above 1,024 texels on a
side are decoded on the CPU but not uploaded. Sound decoding is checked
against digests, not played.
