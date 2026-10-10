# macOS Dolphin lane and cross-host consistency

This adds a second Dolphin lane on an Apple silicon Mac to the HWI-005
evidence loop. The same runner (`tools/wii/run_dolphin.py`) now runs on macOS
and Windows. The same four DOLs, inputs and staged files ran on both hosts.
Every guest-written SD file read back byte-identical across hosts, including
the GX materials CRC and the geometry cull grid. Frame dumps are not
byte-identical across hosts; see the findings below. This is emulator
evidence only; physical Wii behaviour is untested.
[Numeric evidence](2026-10-10-macos-dolphin-lane.json)

## Hosts

| | Windows lane | macOS lane |
|---|---|---|
| OS | Windows 11 Pro 10.0.26200 | macOS 27.0, arm64 (Apple M5 Max, 18 cores, 36 GB) |
| Dolphin | 2609 x64, `Dolphin.exe` `1ed5e3f6…0dac` | 2609 universal, `Contents/MacOS/Dolphin` `b3c369bc…3ccf` |
| Backend | D3D | Vulkan (MoltenVK 1.2.8, bundled with Dolphin) |
| Runner Python | 3.14 | 3.12.15 (Homebrew) |
| SD staging | mtools in WSL (Debian) | mtools 4.0.49, native |

Both hosts run the same Dolphin release and the same stock-limit profile: Wii
mode, no overclock, no cheats, emulation speed 1.0, single core, 1x, CPU EFB
access where the scenario needs it.

## macOS installation

- Dolphin 2609 came from the official disk image
  (`dolphin-2609-universal.dmg`, 41,965,559 bytes, SHA-256
  `b8910a6f8710cbe93b916f5b3867a46e67e49aa8272657d282d4accae359cd6b`),
  downloaded with curl. The file carried no extended attributes. It was
  attached read-only, `Dolphin.app` was copied into a user folder, and the
  image was detached. No administrator rights were used.
- The app is signed by "Developer ID Application: Stichting Dolphin
  Emulator (97835T4369)" and Gatekeeper accepts it as a notarized Developer ID
  app. It carries no quarantine attribute; macOS added only
  `com.apple.provenance`. No Gatekeeper, quarantine or other security setting
  was changed.
- Homebrew installed `python@3.12` 3.12.15 and `mtools` 4.0.49. To satisfy
  `python@3.12` it also installed `ca-certificates` 2026-09-25, `mpdecimal`
  4.0.1 and `openssl@3` 3.6.5.
- With `--user`, Dolphin created no global state: nothing in Application
  Support, Preferences, Caches or Saved Application State.

The owned data that later work needs was copied into a private folder outside
any Git checkout: the Xbox image, the BSP sample, the tag proof and the
section selector manifest. SHA-256 matched on both ends; the hashes are in the
JSON. These runs did not read the Xbox image.

## Launch method on macOS

The runner starts `Dolphin.app/Contents/MacOS/Dolphin` directly as a child
process with stdin closed. That keeps the real exit status, stdout and stderr,
and the timeout kill. This works from an SSH session, whose launchd session
type is `Background`, as long as the same user has a desktop login session;
Dolphin's render window opens on that desktop. `open -W -n` was rejected
because it loses the exit status. `launchctl asuser` needs root and was not
used. A host with no desktop login session is untested.

## Build

Clean commit `f5261f8c` (`source_dirty: false`), build ID `92a310eb7a383c2e`,
devkitPPC GCC 16.1.0, built on Windows with
`python configure.py --wii --wii-devkitpro <root> --wii-probe-frames 1800` and
`ninja wii_probe wii_gx_scene wii_gx_materials wii_geometry_view`. The same
DOL files were copied to the Mac, with equal SHA-256 on both hosts.

| DOL | Bytes | SHA-256 |
|---|---|---|
| probe.dol | 455,424 | `58803ba906ef087cfe3ac9144929c23c0fc903ab0009ed801945d8c4a6348365` |
| gx_scene.dol | 482,656 | `9da29c8f382c7ad5133f5bdeb7f4ca25385b7dd2b7a1c95e747e153a6f490599` |
| gx_materials.dol | 512,224 | `3d4464858608cf0f45605599b85bb8680f2896a11e351f9ba4e0d82e61e7cd60` |
| geometry_view.dol | 547,232 | `04eaf8d86a3bcd14202af64d9bb2c41aa8bdb0aeef07473f99e12889ab38ef72` |

## Runs: two cold launches per target per host

Every batch used a new isolated profile, authored DTM input (`tools/wii/inputs`),
`--dump-frames`, and the host's own exclusive Dolphin lock. The geometry batch
staged the private tag proof, BSP sample and guest manifest (`section.txt`,
the same CRLF bytes as the HWI-016 runs) into a copy of a blank SD image that
Dolphin 2609 created. The other three batches let Dolphin create its own SD
image in the new profile, on both hosts.

| Target | Guest result (both hosts, both runs) | Windows elapsed | macOS elapsed |
|---|---|---|---|
| gx_materials | 196/196 checks, CRC `53843a35` ×4, 17/17 negatives, exit by START | 18.4 s, 16.7 s | 12.2 s, 12.1 s |
| gx_scene | calibration 9/9, orbit/zoom/spin/reset input, exit by START | 11.9 s, 17.5 s | 8.4 s, 8.1 s |
| probe | sentinel 0 → 1 → 2, prior log preserved, exit by START | 6.9 s, 6.6 s | 3.3 s, 3.3 s |
| geometry_view | 4 cycles, CRCs `bad05748`/`db11989c`; cull none `aac744a4`, back `18b17d9a`, front `b9cf71f5`, each identical in all cycles | 27.4 s, 27.1 s | 23.8 s, 23.8 s |

Every run on both hosts: guest pass, persistence pass, host exit `0x0`
(natural, not forced), no Dolphin process left afterwards, runner exit 0.
Earlier Windows records for other builds of the same targets were about 15 s
(gx_materials), 5.7 s (probe) and 26 s (geometry).

The geometry cull table matches HWI-016 exactly. At pose 0, `GX_CULL_NONE`
shows 63 samples (30 lit, 33 unlit), `GX_CULL_BACK` 47 (47 lit) and
`GX_CULL_FRONT` 52 (52 unlit). The guest timings it reports in emulated time
(load 1.70 s, draw 280 µs shaded and about 2,227 µs classified) and the FIFO
peak (77,632 bytes) are also equal across hosts.

**Timing.** Wall time is mostly emulated frames at 60 Hz, because the stock
profile keeps emulation speed at 1.0. The Mac was 3.3–9.4 s faster per
launch (13–54 %). A difference of about 3.5 s appears in every target, so it
is per-launch overhead (startup and teardown) rather than emulation speed. The
Windows times also vary more between the two launches, and other agents were
sharing that host. More throughput comes from running the second lane in
parallel, not from a faster single run.

## Cross-host comparison (same DOLs, same inputs)

| Item | Result |
|---|---|
| Guest SD files read back (logs, counters, sentinel) | Byte-identical for every target and run |
| Staged files after every run | Byte-identical (tags, BSP, manifest) |
| DTM movies | Byte-identical |
| Checker observations (CRCs, cull table, summaries, timings) | Equal |
| Frame dumps (PNG) | Differ; see findings |
| OS stability | Nothing observed on either host |

## Findings

1. **Metal backend fails two GX checks.** With `--backend Metal`, gx_materials
   fails `mip_max_lod_1` and `mip_min_lod_2` in every cycle (188/196, cycle
   CRC `2efb1ade`) in both cold launches. Vulkan and OpenGL pass 196/196
   with CRC `53843a35` (OpenGL was a development check with an earlier
   build). The runner therefore defaults to Vulkan on macOS. This is a
   Dolphin Metal backend difference in texture LOD clamping, not a guest or
   host fault.
2. **Frame dumps differ across hosts, guest EFB readbacks do not.** Decoded
   pixels differ by at most 1/255 in up to 0.52 % of channels in the sampled
   probe, gx_scene, geometry and gx_materials frames. One exception is the last
   gx_materials frame (the trilinear mip view): up to 28/255 in 2.7 % of
   channels. The first frames of gx_materials and geometry_view are
   byte-identical. Frame-dump hashes are only comparable within one host and
   backend. Cross-host comparisons must use guest-side EFB readbacks.
3. **Two existing inspector tests fail on APFS.** On macOS, 293/295
   `tools/wii` tests pass. Two path-replacement guard tests in
   `test_inspect_bsp_residency` and `test_inspect_material_graph` expect a
   "changed" error that APFS does not trigger when a file with equal size and
   mtime replaces the original. Those inspectors were not changed here. The
   runner's own tests pass 50/50 on macOS.

## OS stability (both hosts, read-only, batch windows only)

- **Windows:** System log IDs 41, 1001, 6005, 6006 and 6008, and Application
  faults naming Dolphin. None in any window.
- **macOS:** `kern.boottime` (last boot before every window), kernel panic and
  Dolphin crash reports in the system and user `DiagnosticReports` folders,
  and `log show` over the window for kernel panic, GPU-restart and watchdog
  messages, ReportCrash naming Dolphin, and fault-level Dolphin messages.
  None in any window. A panic would restart the host and end the runner. It
  would appear as a later boot time or panic report, not in that batch's
  record.

Absence of events is a session-scoped observation, not a guarantee.

## Unit tests

| Environment | `test_run_dolphin` | All `tools/wii` tests |
|---|---|---|
| Windows, Python 3.14 | 50/50 | 295/295 |
| WSL Debian, Python 3.13 | – | 295/295 |
| macOS, Python 3.12.15 | 50/50 | 293/295 (finding 3) |

The runner and its tests use no Python 3.12-only f-string syntax. Hosted CI
(Debian, Python 3.11) has not run on this branch, because it is not pushed.

## Not established

- Physical Wii behaviour, and the hardware cost of any timing.
- macOS without a desktop login session, and Intel Macs.
- Whether the Metal LOD difference is a Dolphin defect or a Metal limitation.
- Cross-host identity for anything other than these four DOLs and inputs.
