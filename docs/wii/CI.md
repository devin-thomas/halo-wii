# Wii continuous integration · build-only

[`.github/workflows/wii-build-only.yml`](../../.github/workflows/wii-build-only.yml)
builds the asset-free Wii targets and runs the authored checks on every push
and pull request to `wii` (Markdown-only changes excluded) and on manual
dispatch. It needs no private repository, game data, dump or secret.
Local verification of its commands and of the inspection:
[evidence](evidence/2026-10-10-ci-build-only.md). The hosted run is recorded
separately once it has happened.

## What a green run proves

| Checked | How |
|---|---|
| Toolchain identity | `dkp-pacman -Q`, `check_toolchain.py`, compiler/binutils/library hashes in each `build-info.json` |
| Authored Python checks | `python -B -m unittest discover -s . -p "test_*.py"` in `tools/wii` (synthetic inputs only) |
| Authored host C fixtures | `run_abi_host.py` and `run_cache_address.py` default, `--widget-fixture`, `--bsp-fixture` and `--material-fixture`, compiled with the container's GCC and executed on the Linux x86-64 host |
| Wii compile/link/convert | `configure.py --wii` then `ninja wii_probe wii_gx_scene wii_gx_materials wii_geometry_view wii_memory_strategy wii_media_bench wii_engine`; ELF and DOL structure checks; the engine's generated unsupported-entry-point file is current (`tools/wii/engine_unsupported.py --check`) |
| Publishable artifact set | `package_artifacts.py stage` and `inspect` (below) before upload |

The uploaded artifact `wii-build-only-<commit>` and its `MANIFEST.json` are
labeled **build-only**. A green run is **not** an emulator, gameplay, full
engine ABI, host process lifecycle, persistence or physical Wii result, and it
does not change any capability in [STATUS.md](STATUS.md). Host fixture results
describe the x86-64 host only, not PowerPC execution.

Not run in CI: Dolphin, hardware, any owned-data mode (`--private-*`,
`--stream-file`, `*-goldens`, `inspect_*` against real files), and the
`run_memory_subset.py` diagnostics. The memory subset asserts the original
32-bit `long` and cannot build on an LP64 Linux host; its packet modes also
exit 1 by design to keep the [recorded known failures](evidence/2026-10-07-packet-dispatch.md)
visible, so they are not CI gates.

## Environment

- GitHub-hosted `ubuntu-24.04` runner only. A first step fails the job on any
  self-hosted runner.
- Official image `devkitpro/devkitppc:20260503`, pinned by its index digest
  `sha256:44cb1a920e1ec3ec7c06767493c3b85f8d643d6137cc4661f0201895ac6e4967`
  (Debian bookworm with Python 3.11, Ninja and GCC 12). Package versions inside
  the image are logged and recorded in the manifest at run time.
- `actions/checkout` v7.0.1 and `actions/upload-artifact` v7.0.1, pinned by
  commit; checkout does not persist credentials.
- `permissions: contents: read`; `pull_request`, never `pull_request_target`, so
  fork pull requests get a read-only token and no secrets.

The upstream `build.yml` (desktop/Android builds on every push) is unchanged.

## Artifact allowlist and inspection

`tools/wii/package_artifacts.py stage` copies exactly these files into a new
directory and writes `MANIFEST.json` (label, verified/not-verified lists,
commit, build ID, toolchain packages, per-file SHA-256):

```text
probe.elf  probe.dol  probe.map  build-info.json
gx_scene.elf  gx_scene.dol  gx_scene.map  gx_scene-build-info.json
gx_materials.elf  gx_materials.dol  gx_materials.map  gx_materials-build-info.json
geometry_view.elf  geometry_view.dol  geometry_view.map  geometry_view-build-info.json
memory_strategy.elf  memory_strategy.dol  memory_strategy.map  memory_strategy-build-info.json
media_bench.elf  media_bench.dol  media_bench.map  media_bench-build-info.json
engine.elf  engine.dol  engine.map  engine-build-info.json
MANIFEST.json
```

`inspect` fails the job before upload if:

- any other file, a directory or a link is present, or an allowlisted file is missing;
- an ELF/DOL fails its structure check, a map is not a GNU ld text map, or a
  file exceeds 48 MiB (128 MiB total; the whole-engine ELF keeps its debug information);
- any recorded hash or size differs, build-info scope is unexpected,
  `runtime_verified` is not false, the tree was dirty (`--require-clean`), or
  the manifest label/claims differ from build-only;
- content contains a Windows drive, UNC, MSYS/WSL drive, home or CI workspace
  path, this machine's working/home/temp directories or user names, a supplied
  owner name, a private IPv4 address, key/token/credential patterns, or an
  Xbox image, XBE or Halo cache-header signature.

Findings name only the file, category and offset, so a leaked value is not
repeated into the public log.

In ELF files the generic path shapes (drive, UNC, MSYS/WSL, home and
workspace) are matched by section: allocated sections, string tables,
`.comment`, `.debug_str`, `.debug_line_str` and the directory and file tables
of every `.debug_line` unit (DWARF 2-5). Encoded debug data (`.debug_info`,
line-number programs, location and range lists, frames, abbreviations) and the
symbol table are skipped, because their printable runs are encoding noise that
moves with every code change. This machine's paths and user names, owner names,
secrets and Xbox data signatures are still matched on every byte of every file,
and an ELF whose sections cannot be read is a problem, not a pass. DOL files
hold no debug sections and are scanned whole. Reviewed exceptions are the official install
prefix `/opt/devkitpro/` and devkitPro's own package build roots
(`/home/davem/projects/devkitpro/{pacman-packages,tool-packages}/`), which the
official prebuilt libogc, newlib and libgcc embed for every user.

The build itself avoids host paths: `tools/wii/build.py` compiles with
`-ffile-prefix-map` and rewrites the linker map so the checkout appears as `.`
and the devkitPro root as `/opt/devkitpro` (`path_prefix_map` in
`build-info.json`). Code is unchanged; only the embedded build ID differs.

Content signatures are a backstop, not proof of absence. The main guarantee is
that CI sources are the public checkout and no owned input is ever supplied.

Run the same steps locally (replace the paths):

```sh
python configure.py --wii --wii-devkitpro <devkitPro root>
ninja wii_probe wii_gx_scene wii_gx_materials wii_geometry_view wii_memory_strategy wii_media_bench wii_engine
python -B tools/wii/package_artifacts.py stage --build build/wii --output dist/wii-build-only
python -B tools/wii/package_artifacts.py inspect dist/wii-build-only --require-clean
```

## Trusted owned-data and Dolphin tests stay local

Owned-data, Dolphin and Wii tests are owner-run on a trusted machine, never in
GitHub Actions and never on a runner that public pull requests can reach:

- keep game images, maps, tag blobs, goldens, SD staging and Dolphin profiles
  outside the checkout; generated private builds go to ignored `.local/` or
  `build/` directories and are never uploaded;
- run only reviewed commits, not unreviewed pull-request code, with owned data;
- do not register a self-hosted runner for this repository on a machine that
  can read dumps, credentials or the Wii network;
- publish only sanitized evidence under [evidence/](evidence/): commands,
  versions, hashes and observations, with no local paths, addresses or game bytes.

Runtime results keep their own scope (guest scenario, persistence/restart,
host teardown, OS stability, hardware) and are never summarized by a
build-only badge.
