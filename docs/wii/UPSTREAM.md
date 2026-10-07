# Pinned upstream baseline

Source audit date: 2026-10-07. Integration base:
`4e8ed2f196e0edd1f2830a4de9841686aabbf466` of
[OpenCommunityEdition/OpenCE](https://github.com/OpenCommunityEdition/OpenCE/commit/4e8ed2f196e0edd1f2830a4de9841686aabbf466).
This is source inspection, not a Wii build or runtime compatibility result.

The original planning snapshot was
`2b0327bc80ca38c90894cb56b19652cbe85733ff`. There are 75 reachable commits
between that snapshot and this base. In particular, the older memory numbers
must not be treated as the current desktop configuration.

| Contract | Inspected source | Current value |
| --- | --- | --- |
| Wire version | `port/linux/include/halo_port_limits.h` | 22; previously 17 |
| Session limits | Same header | 128 players / 128 machines; four local slots |
| Address window | `port/linux/src/platform.h` | Desktop 512 MiB; Android 128 MiB |
| Texture cache | `port/linux/include/halo_port_capacity.h` | Desktop 256 MiB; Android 22 MiB |
| Game state | Same header | Base `0x81A00000`; CPU + GPU exactly 20 MiB |
| Xbox tag cache | `source/cache/physical_memory_map.c` | Base `0x803A6000`; exact-address allocation |
| Map recognition | `port/linux/game/cache_file_formats.h` | Xbox version 5; Custom Edition 609 |

Virtual reservation size is not a measured resident working set. None of these
desktop address ranges or capacities is accepted as a Wii allocation policy.
The existing header requires equal simulation capacities across session peers;
silently reducing them does not establish mixed-port compatibility. The latest
Custom Edition loader and large desktop texture cache are upstream enhancements,
separate from the required Xbox CE content baseline.

The decompilation identity remains Xbox build 2342, `cachebeta.exe`, SHA-256
`4cc87b45f721270392a96f1674ed2b5cd4a7bb4355faeab4531d1cf1884d9520`.
The upstream save path stores native memory images and validates allocation/map
identity; it does not supply the proposed portable, versioned Wii save schema.

Python/Ninja generate native desktop/Android builds. Windows additionally uses
LLVM clang/lld, x86 MSVC libraries, a Windows SDK, SDL 3.4.16 and OpenGL.
These are reference-platform dependencies, not prerequisites for the future
asset-free Wii diagnostic. `configure.py --help` currently has no Wii switch.

Preserve the root CC0 notice and each bundled component's notices. Bundled
TOML, Expat, KCP, Monocypher, musl math, zlib, Mbed TLS, miniupnpc, stb,
SMAA, extract-xiso and fonts carry their own license/provenance records.
No Nintendo SDK implementation or reference-game decompilation is a new dependency.

Next proof: an official Wii example, then an asset-free native diagnostic,
followed by actual PPC ABI/endian and measured MEM1/MEM2 tests.
