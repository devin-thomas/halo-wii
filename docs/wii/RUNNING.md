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

1. Refuses to start if any `Dolphin.exe` is already running (unless
   `--allow-concurrent` runs a PID-scoped lane; see
   [concurrent lanes](BUILDING.md#concurrent-dolphin-lanes-hwi-073)), if the output
   directory exists, if the Dolphin hash differs from `--dolphin-sha256`, or
   if the DOL hash differs from the build manifest.
2. Creates a new profile under `<out>/profile`: Wii mode, stock clock and
   memory (no overclock, VI overclock or RAM override), no cheats, normal
   emulation speed, single-core, 1x internal resolution, a standard GC
   controller on port 1, Wii remotes disabled, a writable emulated SD card
   with folder sync off. `--efb-access` adds CPU EFB peeks for GX self-checks.
   By default no host input device is mapped anywhere; see
   [focus- and keyboard-safe launch](#focus--and-keyboard-safe-launch-hwi-077).
3. Authors a DTM from the JSON pad script (one entry per controller poll).
   Dolphin derives a DOL's game ID from its file name (`ID-` plus the stem,
   compared on six bytes) and silently ignores a movie whose ID differs, so
   the runner writes the matching ID. A supplied `--movie` is refused if its
   ID does not match. The authored header carries the largest tick count, so
   Dolphin plays the script until its states run out. Dolphin also ends
   playback once the emulated CPU passes the header's tick count; the earlier
   100,000,000,000 ticks were 137.17 s of the Wii's clock (HWI-081). The record's
   `input.playback_ends` says which applies, also for a supplied `--movie`.
4. Optionally copies a FAT32 image (`--sd-image`, never modified) into the
   profile and stages files with `--stage LOCAL=sd:/path`, using mtools
   (`mmd`, `mcopy -n`) in a WSL distribution on Windows, or natively on macOS.
   Staging only happens when files
   are supplied; staged files are read back before launch and after every run.
   Dolphin creates its own blank image on the first launch when none is given.
5. Runs `Dolphin.exe --user <profile> --batch --video_backend D3D --exec <dol>
   --movie <dtm>` for each cold launch, hidden, with a timeout. A timeout is a
   forced stop and is never treated as completion.
6. Reads guest files back from the SD image with a read-only FAT32 reader and
   applies the scenario checks: `--scenario probe` for the asset-free probe;
   `gx_materials`, `gx_scene` and `geometry_view` (each requires
   `--efb-access`) for the GX materials self-test, the asset-free GX scene and
   the real-geometry diagnostic. Their pad scripts are in `tools/wii/inputs`.
7. On Windows, queries the Windows System log (IDs 41, 1001, 6005, 6006, 6008) and the
   Application log (1000, 1002 naming Dolphin) over the batch window,
   read-only, and records only IDs, providers, times, image/module names and
   exception codes.

`<out>/runtime.json` keeps build identity, guest, persistence, host lifecycle
and OS stability as separate outcomes, with hashes and names rather than local
paths. The runner exits 0 when every outcome is accepted, 1 for a guest or
persistence failure or a missing run, 2 for a refused precondition, 3 for a
host exit that is neither zero nor named with `--tolerated-host-exit` (or a
forced stop, or a teardown check that found the runner's own Dolphin process
tree still present), and 4 when OS instability events are observed. A tolerated code
is still recorded as `nonzero_exit_tolerated` with the actual exit code; it is
never rewritten as zero. The SD image, frame dumps, guest logs and profile stay
in the output directory, outside Git.

When several agents or people share the host, take the host's Dolphin lane
lock around every batch and release it afterwards, even on failure. With one
slot it is exclusive; with counted slots, pass `--allow-concurrent` and
follow the honest-timing rule in
[concurrent lanes](BUILDING.md#concurrent-dolphin-lanes-hwi-073). The runner's
own process check is a guard, not a lock.

### Focus- and keyboard-safe launch (HWI-077)

Dolphin runs on desktops that people are using. By default the runner keeps
it from taking the foreground, keyboard focus or any input device, on both
hosts:

- **Input:** every input config (GC pads, GC keyboards, GBA, free look,
  hotkeys, Wii remotes) is written bound to an absent device
  (`None/0/No Device`). Every control that Dolphin 2609 binds by default is
  written with an empty expression, listed in
  [`dolphin_controls.json`](../../tools/wii/dolphin_controls.json); this
  includes all 144 hotkeys, such as Escape for Stop. Without these files
  Dolphin maps the keyboard to GC pad 1 and to its hotkeys.
  `BackgroundInput`, pause-on-focus-loss, always-on-top and fullscreen are
  off. All emulated input comes from the DTM.
- **GC pads always connected (HWI-081):** each GC pad has
  `Options/Always Connected = True`, a setting rather than a binding. With no
  device bound, Dolphin otherwise reports the pad unplugged whenever the DTM
  is not playing: before a movie starts, after it ends, or in a launch with
  no movie. The old profile's keyboard device kept the pad plugged in. After
  the script ends the guest sees a plugged-in pad with nothing pressed, as it
  did with the old profile.
- **macOS launch environment:** `QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM=1`
  and `QT_MAC_SET_RAISE_PROCESS=0` stop Qt from activating the process.
  `SDL_JOYSTICK_MFI=0` stops SDL from opening a GameController HID client.
  Windows keeps its hidden-window launch (STARTUPINFO `SW_HIDE`).
- **Focus guard:** a helper thread in the runner samples every 0.25 s. On
  macOS it reads the frontmost application with `lsappinfo`, and this run's
  hidden and active state through `NSRunningApplication` (ctypes). On
  Windows it reads `GetForegroundWindow` and `GetGUIThreadInfo`.
  - On macOS it hides this run's Dolphin as soon as it registers.
  - On either host it stops this run's Dolphin if it is ever frontmost,
    active, or holds keyboard focus. The root is stopped through its own
    process handle; a descendant is stopped only by explicit PID from the
    runner's own tree.
  - Another lane's Dolphin, or any other application, is only counted.
  - If the guard cannot start, the runner refuses to launch.
  - `--focus-guard observe` samples only; `--focus-guard off` disables it.
- **Audio:** off by default on macOS (`--audio` turns it on), unchanged on
  Windows (`--no-audio` turns it off). The guest's DSP is emulated either
  way; only the host output changes.

Each run records `focus_guard`: the sample counts, `own_frontmost`,
`own_active`, `own_keyboard_focus`, hide requests, other lanes' Dolphins
seen frontmost, and whether the guard stopped Dolphin. It also records
`input_binding_audit`, which reads back the input configs Dolphin actually
saved and lists any bound device or control (`no_input_bound` is the clean
result).

The batch's `focus_safety` object lists the added environment variables and
`Dolphin.ini` settings, the input files and the controls file hash.

A guard stop is host outcome `focus_guard_stop` and guest failure
`focus_guard_stop_before_natural_exit`, and later launches are skipped.

### Input integrity (HWI-081)

A host replay of a launch replays the guest's own pad, so an unplugged pad
or a lost press would still compare equal. The runner therefore checks what
the guest saw against the DTM, for every guest that logs its pad (the boot
mode's `BOOT_PAD` and `BOOT_PAD2` lines in any read-back file). Each run
records `input_integrity`, and each of these fails the guest outcome:

- `pad_unplugged`: a port the DTM drives read no controller;
- `input_desync`: a state the script does not give at that point, such as an
  unscripted press or input on a port the DTM does not drive;
- `input_missed`: a scripted press the guest passed over between two states
  it did see (a lower bound);
- `gc_pad_always_connected_not_kept`: Dolphin's saved `GCPadNew.ini` lacks the
  setting above.

Buttons are compared bit for bit. Sticks and triggers are compared as
neutral or not, because the guest sees them after libogc's origin and
clamping. Once the script's states run out, a plugged-in pad with nothing
pressed is expected. Presses the launch ended before reaching are counted
(`scripted_states_not_reached`), not failed. A guest without pad lines is
reported as not applicable.

`run_boot_host.py` records the Wii launch's unplugs in `host.json`
(`wii_pad_unplugged`) and exits 5 after an otherwise successful replay that
followed one. `--allow-unplugged-pad` accepts it.

`--no-focus-safe` writes the earlier profile byte for byte, for comparison
only. Guest output is byte-identical with and without these changes; see
[the focus-safe runner record](evidence/2026-10-11-focus-safe-runner.md).

Unit tests for the pure parts (DTM authoring, profile generation, FAT32
read-back, staging commands, event classification, scenario checks, PID-scoped
lanes, and the focus guard with mocked focus queries):

```powershell
python -B -m unittest discover -s tools/wii -p "test_run_dolphin*.py"
```

### macOS lane

The same runner runs on macOS (qualified on Apple silicon, macOS 27, with
the official notarized Dolphin 2609 universal build; see
[the macOS lane record](evidence/2026-10-10-macos-dolphin-lane.md)). It needs
Python 3.11 or later and mtools (for example `brew install python@3.12 mtools`).
Install Dolphin by copying `Dolphin.app` out of the official disk image into a
user folder; no administrator rights, Gatekeeper or quarantine changes are
needed for a notarized build.

```sh
python3.12 -B tools/wii/run_dolphin.py   --dolphin <folder>/Dolphin.app --dolphin-sha256 <sha256 of Contents/MacOS/Dolphin>   --dol <gx_materials.dol> --build-info <gx_materials-build-info.json>   --out <new directory outside Git> --runs 2 --timeout 120 --dump-frames   --efb-access --input-script tools/wii/inputs/gx-materials.json --scenario gx_materials
```

Differences from Windows:

- `--dolphin` accepts the `.app` bundle and runs `Contents/MacOS/Dolphin`.
  `--dolphin-sha256` pins that executable only; the bundle's code signature
  covers the rest.
- The default backend is `Vulkan` (MoltenVK, bundled with Dolphin). The `Metal`
  backend fails two mip-LOD checks of the GX materials self-test
  (`mip_max_lod_1`, `mip_min_lod_2`, CRC `2efb1ade`) that pass under Vulkan,
  OpenGL and Windows D3D. Do not use Metal for EFB evidence.
- Process ownership and the exclusivity check use `ps`; staging runs native mtools.
- Launch: Dolphin is executed directly by the runner, which keeps the real
  exit status, stdout/stderr and timeout control. This works from an SSH
  session as long as the same user has an active desktop login session. With
  the default focus guard the process is hidden on that desktop as soon as it
  registers and never becomes frontmost (see
  [focus- and keyboard-safe launch](#focus--and-keyboard-safe-launch-hwi-077)).
  It still has a Dock icon and a Cmd-Tab entry; switching to it on purpose
  gets it stopped. `open -W -n` was not used because it
  loses the exit status. A host with no desktop login session is untested.
- `--user` keeps all Dolphin state in the new profile; no global Dolphin
  configuration is created in the user's Library.
- Dolphin creates its own blank SD image in the profile when no `--sd-image`
  is given. Staging needs an existing image; use a Dolphin-created blank image
  (FAT32, 128 MiB, label `DOLPHINSD`).
- OS stability (read-only, batch window only): `kern.boottime` (a boot after
  the window start), file names and times in the system and user
  `DiagnosticReports` folders (kernel panic reports and Dolphin crash reports;
  only the kind is kept), and `log show` over the window for kernel
  panic/GPU-restart/watchdog messages, ReportCrash naming Dolphin and
  fault-level messages from Dolphin. A panic restarts the host and ends the
  runner, so it would show up as a later boot time or panic report rather than
  in that batch's record.

On a shared host, wrap every batch in the host's own Dolphin lane lock,
as on Windows.

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
