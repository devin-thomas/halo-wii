# Vendored third-party decoders (HWI-008D)

These files are compiled into the Wii movie and audio benchmark
(`port/wii/media_bench`) and are the decoders chosen for the game's movies.
Each was fetched from its official upstream repository at a pinned commit,
into a new empty directory, and read before it was built. The files are
stored byte for byte as published (`.gitattributes` turns off line-ending
conversion), so the SHA-256 values below can be checked at any time;
`tools/wii/test_third_party.py` does that.

| File | Upstream | Commit (date) | SHA-256 |
|---|---|---|---|
| `pl_mpeg/pl_mpeg.h` | <https://github.com/phoboslab/pl_mpeg> | `c871f2be022ece7ef4f64230b4fb8e1fb9eb6023` (2025-12-30) | `3a8cb30c83c2a1147719c30fe0c8b93da2987aa43140077c575b39aaa75fc2c9` |
| `stb/stb_image.h` (v2.30) | <https://github.com/nothings/stb> | `2c980bb59875b0d32144a71867fbdebb2f77cd20` (2026-08-02) | `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3` |

Derived files:

| File | What it is | SHA-256 |
|---|---|---|
| `pl_mpeg/pl_mpeg_wii.h` | `pl_mpeg.h` plus `pl_mpeg_wii.patch`; this is the copy the build uses | `a4d72bd724a9c0ab92516a8d07e096c9d45b3c55db5bc843e20ddc053a8e781e` |
| `pl_mpeg/pl_mpeg_wii.patch` | The HWI-008D speed patch, authored for this port (MIT, like the file it changes) | `231e28e809b26a395194419f6968caeefab591a096364ead91792bc64c530d7c` |
| `pl_mpeg/LICENSE` | The MIT notice for pl_mpeg | `a20c1ebbaec088ee9226f46ec201b74f56d5dd95edc71eaa5470443ebd8b4410` |

## Licences and obligations

**pl_mpeg** (MPEG-1 video, MP2 audio, MPEG program stream demuxer), by
Dominic Szablewski, is MIT. The pinned header declares
`SPDX-License-Identifier: MIT` and names the author. The repository has no
separate licence file. Upstream revisions up to `3198122a` (2024-09-07)
carried the full MIT text with "Copyright (c) 2019 Dominic Szablewski"; the
2024-10 revisions replaced it with the SPDX identifier. `pl_mpeg/LICENSE`
reproduces that MIT text so the notice travels with the code.

**stb_image** (only its baseline and progressive JPEG decoder is compiled,
with `STBI_ONLY_JPEG`), by Sean Barrett, is offered under MIT or the
Unlicense (public domain), at the user's choice. The full text is at the end
of `stb_image.h`. This fork uses it under MIT.

Both licences are permissive and compatible with the GPL and with the
fork's CC0 code. For any binary or source redistribution, keep the
copyright and permission notices: the two header files and
`pl_mpeg/LICENSE`, and the same notices in a release's third-party notice
file. The patch states that `pl_mpeg_wii.h` is modified, and how. No other
obligation applies: no source offer, no patent licence, no attribution in
the user interface.

## Patents

- MPEG-1 video and MPEG-1 Audio Layer II (MP2) are no longer covered by
  essential patents. The Wikipedia MPEG-1 article (read 2026-10-10) states
  that all declared video patents have expired, and that the US patent
  5,214,678 cited for Layer II, filed in 1990, has expired. The US Library
  of Congress format description for MPEG-1 lists no known licensing or
  patent constraint. The pl_mpeg README says the same.
- Baseline (Huffman) JPEG: the Forgent '672 patent's term ended in 2006,
  according to the Wikipedia JPEG article. The arithmetic-coding options are
  not used by stb_image, which supports only Huffman coding.
- This is engineering due diligence, not legal advice.

## Not vendored

- The Xbox ADPCM decoder is authored in this repository
  (`port/wii/media_bench/media_core.c`). It follows the published IMA ADPCM
  algorithm, the Linux port's mixer and `tools/wii/media_formats.py`.
- FFmpeg is only a host tool that you supply for import-time transcoding.
  No FFmpeg code is linked into a Wii binary.
- The proprietary RAD Bink SDK and Nintendo's THP and DSP-ADPCM SDK
  components are not used.
