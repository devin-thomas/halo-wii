# Stock Dolphin evidence loop (HWI-005)

A reusable runner, `tools/wii/run_dolphin.py`, now owns the Dolphin launch and
evidence loop that earlier diagnostics ran with one-off local scripts. It was
qualified on the current asset-free probe. Build identity, guest scenario,
persistence, host lifecycle and OS stability are recorded as separate outcomes.
[Full records](2026-10-10-dolphin-evidence-loop.json)

## Build

Clean source `8459c3ebd701b6e67bfb1f31cdf813960b505b7a` (`source_dirty: false`),
build ID `c3d7a4ebfba23d21`, devkitPPC GCC 16.1.0, strict C11
`-Wall -Wextra -Werror`, auto-exit backstop 1,800 frames.

```text
python configure.py --wii --wii-devkitpro <devkitPro root> --wii-probe-frames 1800
ninja wii_probe
```

| Artifact | Bytes | SHA-256 |
|---|---|---|
| probe.dol | 455,424 | `95d7cd1c2c1f55fb88571d470ac3b1754615e5e6a5b397dd7e5d5fac5286359d` |
| probe.elf | 2,285,436 | `77b89e01e18055f17766b1e5555fe17f2e65683f49159197c8f8d7e6a03188ad` |
| probe.map | 763,579 | `9fa08320f1eaad479ad52a2388b3cc67bcf754fcc3e2078749c7782bc48c189c` |

This is not the original 2026-10-07 probe (`0f01e11c08c44932`). The current probe
also prints the ABI fixture report.

## Emulator and input

- Stock Dolphin, `Dolphin.exe` SHA-256
  `1ed5e3f63f08b672133dade2c570e87ea4fa7fac2f6a65a118cc300a18dc0dac`.
- New isolated Wii-mode profile per batch. Stock clock and memory, no
  overclock, VI overclock, RAM override or cheats. Emulation speed 1.0,
  single-core, D3D at 1x. Standard GC controller on port 1, Wii remotes
  disabled. Writable emulated SD card with folder sync off. Frame dumps on.
  CPU EFB access was not enabled; the probe does not read the EFB.
- Input: `tools/wii/inputs/probe-start.json` authored into a DTM
  (SHA-256 `829e51ed1cfac0f9620a691f84a48734f5602071a62d3817c08783bad0dfb229`,
  game ID `ID-pro`, 1,512 polls). The script holds the pad neutral, then
  A + left trigger + stick right, then presses START. This is virtual input,
  not a human or a physical controller.
- Launch shape: `Dolphin.exe --user <new profile> --batch --video_backend D3D
  --exec probe.dol --movie input.dtm`, 120 s timeout. The caller held an
  exclusive host-wide Dolphin lock for each batch.

## HWI-005 scenario: two cold launches (`final-v1`)

Runner at `8459c3eb`. The profile was new and Dolphin created a blank SD image
on the first launch, so the sentinel started absent (count 0).

| Outcome | Run 1 | Run 2 |
|---|---|---|
| Build ID in SD log and on screen | `c3d7a4ebfba23d21` | `c3d7a4ebfba23d21` |
| Video | 640x480, VI mode 0 | 640x480, VI mode 0 |
| MEM1 arena (after SD init) | `0x801c6000..0x81800000`, 23,306,240 B | same |
| MEM2 arena (after SD init) | `0x90002000..0x933c6f60`, 54,284,128 B | same |
| Timer | 146 frames, 72 ticks at 30 Hz (0.493 per frame) | same |
| Input | pad 0 connected, activity seen, `exit=pad_start` | same |
| ABI report | 3 lines, `abi=0`, no `ABI FAIL` | same |
| Guest scope | **passed** | **passed** |
| Sentinel read back | `halo-wii-probe-v1 1` | `halo-wii-probe-v1 2` |
| `previous_runs` in guest log | 0 | 1 |
| Prior log preserved and appended | n/a | yes |
| Persistence | **passed** | **passed** |
| Host exit | `0x00000000`, natural | `0x00000000`, natural |
| Forced stop | no | no |
| Elapsed | 5.76 s | 5.63 s |
| Dolphin processes left after exit | 0 | 0 |

**Screen.** Dolphin dumped one frame per run, because the probe draws to the
XFB through the libogc console rather than GX copies. Both frames show
`Build: c3d7a4ebfba23d21`, `Video: 640x480 mode=0`, the arenas, `SD status=1`,
`previous probe runs=0` (run 1) and `=1` (run 2), PAD0 present, and
`EXIT pad_start`. The on-screen arenas (`0x8019c000`, 23,478,272 bytes) are
printed before SD initialisation, so they differ from the logged values,
which are printed after it. Frame SHA-256: run 1 `b4d0c8d4…f2ad`, run 2
`14d85428…123a`. The frames stay local.

**OS stability.** Read-only event-log query over the batch window
(`2026-10-10T12:18:25Z` to `12:18:47Z`): no Kernel-Power 41, bugcheck 1001,
event-log 6005/6006/6008, or Dolphin application fault/hang 1000/1002 events.
The last boot was 2026-10-08, before this session. A second query over the
whole session (11:00Z to 12:23Z) also found none. This is an observation for
these windows only, not a guarantee.

**Host lifecycle.** Both launches of this DOL exited `0x0` naturally. That is
scoped to this binary and configuration. It does not establish that the
original probe's `0xc0000409` teardown fault is fixed; that remains the
deferred HWI-050. The batch was run with `--tolerated-host-exit 0xc0000409`, so
that code would have been disclosed as `nonzero_exit_tolerated`. It did not
occur.

## Staging and continuation (`staging-v1`)

Runner at `5106505b`. A new profile received a copy of the `final-v1` SD image
(SHA-256 `95144144…a2a0`, unchanged afterwards). The runner staged one 34-byte
text file to `sd:/halo-wii-stage/check.txt` with `mmd` and `mcopy -n` (mtools
4.0.48, WSL Debian); both exited 0, and the file read back identical before
launch. One cold launch:

- The guest continued from the existing sentinel: `previous_runs=2`, and the
  sentinel read back `halo-wii-probe-v1 3`.
- The earlier log was preserved and appended.
- The staged file was unchanged after the launch.
- Guest, persistence and OS outcomes passed. Host exit `0x0`, natural, 6.04 s.

## Invalid-sentinel negative control (`invalid-sentinel-v1`)

Runner at `7d467f37`. The input image was prepared outside the runner. It is a
copy of the `staging-v1` result with `sentinel.txt` overwritten as
`halo-wii-probe-v1 x` using `mcopy -o` (image SHA-256 `50b70651…4be0`). One cold
launch:

- The guest refused storage. The screen shows
  `Invalid sentinel; preserved without overwrite`, `SD status=-1` and
  `EXIT pad_start`.
- The invalid sentinel, the existing 1,491-byte log and the staged file were
  all byte-identical afterwards.
- Persistence (preservation): **passed**.
- The guest wrote no SD report by design, so its SD scope is recorded as
  **not evaluated**.
- Host exit `0x0`, natural, 6.52 s. No OS events.

## Runner checks

`python -B -m unittest discover -s tools/wii -p test_run_dolphin.py` runs
27 tests. They cover:

- DTM layout and the game-ID rule.
- Refusal of a mismatched movie.
- Stock-limit profile checks.
- Authored FAT32 images with long names, multi-cluster files, missing paths and
  looping chains.
- Path traversal rejection and WSL staging commands.
- Exit-code recording, including `0xc0000409` → signed `-1073740791`.
- Event classification, which keeps OS instability separate from Dolphin
  application faults and strips paths.
- Probe pass and failure cases.
- Exit-status precedence.

The event query was also checked over a wider window that contains an
unrelated, pre-existing 2026-10-08 unexpected restart (Kernel-Power 41, 6008).
The classifier reported it as instability.

## Not established

- Physical Wii, real controllers and SD hardware.
- Human input.
- OS stability outside the queried windows.
- Any Halo data, rendering or gameplay.
- The original probe's teardown fault (HWI-050), which this build neither
  reproduces nor resolves.
