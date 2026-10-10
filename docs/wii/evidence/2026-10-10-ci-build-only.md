# Build-only CI and artifact inspection (HWI-009)

Adds the [build-only workflow](../../../.github/workflows/wii-build-only.yml),
the allowlisted artifact stage/inspection and host-path normalization of debug
info and linker maps. [Numeric evidence](2026-10-10-ci-build-only.json) ·
[CI policy](../CI.md)

**Scope: build-only.** Nothing here is an emulator, gameplay, engine ABI, host
lifecycle, persistence or hardware result. No Dolphin or Wii run was made.

**The hosted GitHub Actions run is unverified.** The workflow has not run on
GitHub yet; it first runs when this commit is pushed to `wii` or dispatched.
In particular the official image's package versions, its GCC 12/Python 3.11
behavior with the PPC build, `dkp-pacman -Q`, checkout ownership and the
upload step are unobserved until then.

## Local checks of the workflow's commands

Source `f52bb4c3191cf92dcb078c8b09fc5725794f5129`, clean tree. Windows run
with the existing devkitPro installation (devkitPPC r50-1, GCC 16.1.0, binutils
2.46.0, libogc 3.1.0-1, libfat-ogc 2.1.0-4, gamecube-tools 1.0.7-1), Ninja
1.13.2, MSYS2 UCRT64 GCC 16.2.0 as host compiler:

| Step | Result |
|---|---|
| `pacman -Q`, `check_toolchain.py --devkitpro <root>` | exit 0 |
| `python -B -m unittest discover -s . -p "test_*.py"` (tools/wii), CPython 3.14.0 and 3.11.4 | 232 tests OK |
| same, MSYS2 UCRT64 Python 3.14.7 | **2 failures** (below) |
| `run_abi_host.py`, `run_cache_address.py` default/`--widget-fixture`/`--bsp-fixture`/`--material-fixture` | exit 0, `execution_pass` |
| `configure.py --wii`, `ninja wii_probe wii_gx_scene wii_geometry_view` | exit 0, 20/20 |
| `package_artifacts.py stage … --toolchain-packages <pacman -Q>` | 13 files |
| `package_artifacts.py inspect dist/wii-build-only --require-clean --forbid-text <owner>` | PASS, 0 problems, 0 findings |

Debian 12 (bookworm, the image's base) with GCC 12.2.0 and Python 3.11.2, from
a fresh clone of the same commit: unit tests 232 OK; the ABI host fixture and
all four cache fixtures exit 0 with `execution_pass`; the tree stays clean; the
inspection passes on the Windows-built set. No devkitPPC was available there,
so the Linux PPC build is not reproduced locally.

The two UCRT64-only failures are existing tests, unchanged by this work
(`test_inspect_cache_residency` and `test_xiso_inventory`): they compare
`Path.iterdir()` results whose separator spelling differs under MSYS2 Python.
They pass under python.org CPython and on Linux. CI does not use MSYS2.

Integration check: `wii` moved to `d0c55ec7` meanwhile. Its merge with this
branch is conflict-free, and the merged tree's 259 unit tests (including the
new `test_run_dolphin.py`) pass on Windows CPython 3.14.0 and Debian 12
Python 3.11.2. The merged tree was not rebuilt for PPC.

## Artifacts

All three build-info files: build ID `25eb7e1f3e12fc78`, `runtime_verified:
false`, `path_prefix_map` checkout `.` and devkitPro `/opt/devkitpro`.

| File | Bytes | SHA-256 |
|---|---|---|
| probe.dol | 455,392 | `a601d8cc40a9a389afa18aaf30f67f71847e9670c5c393d51e2ab809bd730d3e` |
| probe.elf | 2,284,764 | `8f1b1f429b54deff540d2b666d73c8c134c80ea59a6ecd3dfaf3271bacf46f0b` |
| probe.map | 655,962 | `bb8582ad5837ffcd4f31a610195f06d7c3265dd28213e4edf09f1a70f5f82e46` |
| gx_scene.dol | 482,592 | `bed01e0fbfc9ea1bff3721dce2c39b7e4c390f575796c904a0d2c1fc85e6e24a` |
| gx_scene.elf | 2,361,184 | `5004f89a308d84465a26f15fc81701fbd940b2fcac02d7637eaf04e1ba15692a` |
| gx_scene.map | 686,764 | `49338b2815f9643bbf25e5c1400a7b8d0b0dfe17aad0cc470f58513a57f4a3df` |
| geometry_view.dol | 543,072 | `ab822198f2786b85175a8456eb1f8da19cb1333f2acc65ca09f38a68ff42980b` |
| geometry_view.elf | 2,607,980 | `eb167042306a48d266ca24929e0fa4b3a290efb9a013e14c25994c5cda23a6bf` |
| geometry_view.map | 735,436 | `e16d073bae3bcf4d9335d2679afbae36329db3ba04511c29bb56e994cb8a90e7` |

**Path independence.** A second clean clone at a different path containing a
space produced byte-identical ELF, DOL, map and build-info files, and a clean
rebuild in place did too. This is one machine and one toolchain; reproducing
these hashes in the CI image is not claimed.

**Code unchanged by path normalization.** Before the change (same sources,
previous `build.py`), each DOL differed from the new one only in its 16-byte
embedded build ID: zero differing bytes after substituting the ID. ELF/map
hashes change because debug info and the map no longer name host paths.

## The inspection discriminates

| Input | Result |
|---|---|
| The clean staged set (control) | PASS, exit 0 |
| Real artifacts built before path normalization | FAIL, exit 1: 3 problems (no prefix map), 14,642 findings: local drive paths in all three ELFs and maps, the checkout path in each ELF |
| Clean set + the real `local-config.json` + an object directory | FAIL: unlisted file, directory |
| Clean set, map with appended token, drive path and private address | FAIL: 2 hash mismatches, `github-token`, `windows-drive-path`, `private-network-address`; the token is not printed |

Unit tests (`test_package_artifacts.py`, 18) cover each rejection category on
synthetic ELF/DOL/map sets, including Xbox image, XBE and Halo cache-header
signatures, runtime/hardware claims, dirty sources, size caps and the reviewed
devkitPro path exceptions, plus link-map normalization.

Commit `7884fda8` later changed only `test_package_artifacts.py` (synthetic secret
shapes assembled at run time); its 18 tests still pass. Builds of later commits
embed a different build ID.

## Upstream workflow

`.github/workflows/build.yml` is unchanged (no diff against `wii`). It still
runs desktop/Android builds on pushes, as upstream intends.

## Not covered

Hosted run; PPC build inside the pinned image; Dolphin, hardware and any
owned-data test; `run_memory_subset.py` (LP64 hosts cannot build it, and its
packet modes exit 1 by design to keep known failures visible).
