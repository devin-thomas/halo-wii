# Owned maps: existing 32-bit Linux strict validation

All 24 privately staged maps pass the existing `map_validate --strict` tool
with zero corrections, zero refusals and no timeouts. This advances the earlier
[read-only inventory/header gate](2026-10-07-owned-xiso-intake.md) through the
existing desktop decompression and tag/BSP checker. It does not establish asset
conversion, canonical dump authenticity, gameplay or Wii memory/PPC loading.

The source is an isolated archive of public commit
`de8768ab7d7fd854e6c66a59948336285d2444a8`. Debian 13/trixie WSL uses Clang 19.1.7,
Python 3.13.5 and Ninja 1.12.1. Ordinary dependencies `clang`, `lld`, `python3`,
`ninja-build`, `gcc-multilib`, `libc6-dev-i386` installed with
`--no-install-recommends`; update, simulation and installation exit 0. The
repository supplies zlib; no system-zlib, SDL, graphics/audio or full-game build
is needed for this target. Production sources, earlier Wii build files, Dolphin
settings and existing evidence remain unchanged.

```sh
python3 configure.py --linux-cc=clang-19 --pgo=off --lto=off --portable --android-ndk=/dev/null
ninja -n -j1 build/linux/map_validate
ninja -j1 build/linux/map_validate
build/linux/map_validate --strict <external-private-maps>/*.map
```

Run the build commands inside a separate source snapshot, and validation with
the private data path. Actual validation invoked each map separately, preserving
each stdout/stderr, hash, status and timing. Configure disables Android discovery,
PGO and LTO. Twenty Ninja steps build two semantics headers, 17 objects and the
link, single-job. The ELF is 32-bit little-endian i386, SHA256
`305f5b39a074ba05a1d5ff60e2fdca760763faed779c15819baa4e2cd5a08934`. All build stages exit 0.

Private staging copies exactly 24 maps / 1,860,638,720 bytes from independently
reviewed inventory spans. It rechecks the full image hash and each header, writes
exclusive files to a new partial directory and renames only after completion.
The source remains read-only; every staged file's read-back SHA256 matches before
validation. Staging takes 7.262s. Image, map bytes, full inventories, staging
manifests and raw logs stay outside both repositories; public records contain
sanitized identities/counts, not game bytes or private paths.

For these 24 stored/decompressed-length mismatches, the existing
[map reader](../../../tools/map_validate.c#L204) inflates after the 2048-byte
header with zlib and requires the exact declared output length. The checker then
loads tags at its desktop fixed address, validates scenario/tag references and
the selected structure BSPs, and counts corrections. Strict mode refuses any
correction. Canonical Xbox cache checksums are not explicitly compared by this
path; source authenticity and exhaustive semantic completeness remain open.
No fuzzing, script/geometry/audio/movie conversion or game execution ran.

The largest validated declared map is 279,763,456 bytes; largest tag-data extent
16,995,988 bytes. Total declared map bytes 2,978,974,720. These are data sizes,
not measured Wii resident arenas. The desktop reader allocates the whole inflated
map plus compressed input and a tag cache. Its success cannot qualify a Wii
memory/address/endian strategy. HWI-005/006 remain blocked, while HWI-007/008
retain their full outstanding acceptance criteria.

The [sanitized record](2026-10-07-owned-cache-validation.json) retains compiler/
package identities, exact generated commands, source/archive/binary hashes,
24 per-run statuses, staging/read-back identity and the separate qualification
limits. The next foundation gate is the measured address/capacity strategy and
PPC asset interpretation, before deterministic conversion or gameplay claims.

An independent review verifies all 2,338 archived source files, the compiled ELF,
all 24 staged hashes and every strict log. Separate maximum-declared-map and UI
runs again exit 0 with zero corrections, unchanged input hashes/metadata and no
timeouts. The BSP loop caps at 32, while the scenario schema limits the block to
16 and reports excess as a correction; strict mode rejects such corrections.
These results retain the checker scope and do not establish full content or Wii
acceptance. The independent reviewer did not repeat the original full-image hash.
