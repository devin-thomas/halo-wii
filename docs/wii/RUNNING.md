# Running and deploying

**No Wii executable is included with these planning documents.** Begin with an asset-free diagnostic built by the implemented probe target. Game data import, package paths and loader behavior must be implemented and validated before a game-run recipe is marked verified.

## Dolphin loop

Use a pinned Dolphin build and launch the Wii DOL/ELF through the options that build actually supports. Record emulator version, configuration, controller mapping, video/backend settings and binary hash. Confirm the diagnostic's visible build identifier; do not mistake a stale executable or savestate for the new build. Run save/persistence checks from cold application starts, not only savestates.

A Dolphin pass supports emulator development. It is not a physical-Wii pass, especially for memory/cache behavior, storage, real controllers and sensors, networking or performance. Keep [status](STATUS.md) scoped to the tested feature and binary.

### Supported launch and evidence runner

`tools/wii/run_dolphin.py` is the supported way to run a Wii DOL in stock
Dolphin and record evidence. It was qualified with stock Dolphin
(`Dolphin.exe` SHA-256 `1ed5e3f6…0dac`) on Windows 11; see
[the evidence loop record](evidence/2026-10-10-dolphin-evidence-loop.md).

```powershell
python configure.py --wii --wii-devkitpro <devkitPro root> --wii-probe-frames 1800
ninja wii_probe
python -B tools/wii/run_dolphin.py `
  --dolphin <stock Dolphin.exe> --dolphin-sha256 <pinned sha256> `
  --dol build/wii/probe.dol --build-info build/wii/build-info.json `
  --out <new local directory outside Git> --runs 2 --timeout 120 --dump-frames `
  --input-script tools/wii/inputs/probe-start.json --scenario probe `
  --tolerated-host-exit 0xc0000409
```

What one invocation does:

1. Refuses to start if any `Dolphin.exe` is already running, if the output
   directory exists, if the Dolphin hash differs from `--dolphin-sha256`, or
   if the DOL hash differs from the build manifest.
2. Creates a new profile under `<out>/profile`: Wii mode, stock clock and
   memory (no overclock, VI overclock or RAM override), no cheats, normal
   emulation speed, single-core, 1x internal resolution, a standard GC
   controller on port 1, Wii remotes disabled, a writable emulated SD card
   with folder sync off. `--efb-access` adds CPU EFB peeks for GX self-checks.
3. Authors a DTM from the JSON pad script (one entry per controller poll).
   Dolphin derives a DOL's game ID from its file name (`ID-` plus the stem,
   compared on six bytes) and silently ignores a movie whose ID differs, so
   the runner writes the matching ID. A supplied `--movie` is refused if its
   ID does not match.
4. Optionally copies a FAT32 image (`--sd-image`, never modified) into the
   profile and stages files with `--stage LOCAL=sd:/path`, using mtools
   (`mmd`, `mcopy -n`) in a WSL distribution. Staging only happens when files
   are supplied; staged files are read back before launch and after every run.
   Dolphin creates its own blank image on the first launch when none is given.
5. Runs `Dolphin.exe --user <profile> --batch --video_backend D3D --exec <dol>
   --movie <dtm>` for each cold launch, hidden, with a timeout. A timeout is a
   forced stop and is never treated as completion.
6. Reads guest files back from the SD image with a read-only FAT32 reader and
   applies the scenario checks (`--scenario probe` for the asset-free probe;
   `--scenario gx_materials`, which requires `--efb-access`, for the GX
   materials self-test in `port/wii/gx_materials`).
7. Queries the Windows System log (IDs 41, 1001, 6005, 6006, 6008) and the
   Application log (1000, 1002 naming Dolphin) over the batch window,
   read-only, and records only IDs, providers, times, image/module names and
   exception codes.

`<out>/runtime.json` keeps build identity, guest, persistence, host lifecycle
and OS stability as separate outcomes, with hashes and names rather than local
paths. The runner exits 0 when every outcome is accepted, 1 for a guest or
persistence failure or a missing run, 2 for a refused precondition, 3 for a
host exit that is neither zero nor named with `--tolerated-host-exit` (or a
forced stop), and 4 when OS instability events are observed. A tolerated code
is still recorded as `nonzero_exit_tolerated` with the actual exit code; it is
never rewritten as zero. The SD image, frame dumps, guest logs and profile stay
in the output directory, outside Git.

When several agents or people share the host, take an exclusive host-wide
Dolphin lock around every batch and release it afterwards, even on failure.
The runner's own process check is a guard, not a lock.

Unit tests for the pure parts (DTM authoring, profile generation, FAT32
read-back, staging commands, event classification, probe and gx_materials checks):

```powershell
python -B -m unittest discover -s tools/wii -p test_run_dolphin.py
```

## Physical console loop

Use an already working Homebrew Channel setup. Inventory the actual Wii model, controller ports, storage and network access before changing anything. These documents do not authorize installing channels/IOS, updating firmware, formatting media or replacing an existing homebrew setup.

1. Start Homebrew Channel and confirm its network-ready state and the intended console's local address.
2. Select an already-built diagnostic, verify its hash and send it with `wiiload`.
3. Confirm its build identifier on the console, exercise the scenario, save/retrieve the log, and return cleanly to HBC.
4. Repeat, including a cold boot. Record what still needs physical button input.

The documented underlying interface is `WIILOAD=tcp:host` followed by `wiiload <executable>`; see [Wiiload](https://wiibrew.org/wiki/Wiiload). It is executable loading while the receiver is available, not SSH, a generic filesystem service, remote power control or recovery from every crash.

A helper is provided at [tools/wii](../../tools/wii/README.md). Run it with an explicit console address and executable path. Its success means the sending process succeeded, **not** that the console ran the scenario correctly.

## Data and evidence are separate

Large source/converted game data must not be embedded in every executable delivery. Stage initial assets using a verified supported medium and path. Subsequently use a separately reviewed LAN-only, project-scoped transfer/log mechanism. Never assume wiiload makes the whole SD/USB filesystem remotely accessible.

The implementation must preserve the previous valid cache/save during interrupted updates, validate a project sentinel file's round-trip hash, retrieve run logs without swapping media, and continue gameplay if a log receiver is absent. Identify the exact mechanism before claiming this loop works. Keep unrelated applications and saves untouched.

## Exit and recovery

Provide a tested safe return path, reset/power callbacks, controller-disconnect handling and bounded diagnostics. A failed network launch may require returning physically to HBC. Do not retry against arbitrary LAN devices, open router ports or weaken the main network's security to hide a configuration failure.
