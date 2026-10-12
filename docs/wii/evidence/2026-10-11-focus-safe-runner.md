# Focus- and keyboard-safe Dolphin runner (HWI-077)

`tools/wii/run_dolphin.py` now keeps Dolphin out of the way of whoever is
using the host, by default, on macOS and Windows. Dolphin must never become
the frontmost application, never take keyboard focus, and never map a
keyboard, mouse or controller. All emulated input comes from the runner's
DTM.

In 24 focus-safe proof launches across three hosts, the runner's own
Dolphin was:

- frontmost in 0 samples;
- active or holding keyboard focus in 0 samples;
- named in 0 WindowServer front switches, 0 keyboard-focus records and
  0 HID event-system connections (macOS).

Every launch read back its own input configs with no bound device or
control. Guest output was byte-identical to 10 launches of the previous
runner and to the HWI-073 and HWI-075 records.

[Numeric evidence](2026-10-11-focus-safe-runner.json)

## The problem

On Research (the Mac Studio, which is also a desktop in use), the standard
runner's Dolphin becomes the frontmost, active application. HWI-075 measured
this in 30 of 30 samples. It happened again throughout this work: another
agent's standard-runner Dolphin was frontmost and active, with a window on
screen, in every one of the 687 samples taken on Research. The WindowServer
gave it the keyboard focus.

The runner also mapped host input by default on both hosts. With no input
configs written, Dolphin 2609 binds these to the keyboard:

- GC pad 1, the GC keyboards, the GBA and free look;
- its hotkeys, including Escape for Stop, F10 and the save-state keys.

Titan's previous profile, read back after a run, had 13 sections bound to
`DInput/0/Keyboard Mouse` and 140 controls bound. The hotkeys were also
active in memory; Dolphin does not save them.

HWI-075 fixed both on Command (the MacBook Air) with a wrapper around the
runner. This ticket moves that method into the runner and makes it the
default.

## What changed

All in `tools/wii/run_dolphin.py` and the new `tools/wii/dolphin_controls.json`
(commits `a8818e1d`, `e7a2d3c3`, `283719a6`):

- **Input, both hosts.** Every input config is written bound to the absent
  device `None/0/No Device`:
  - `GCPadNew.ini`, `GCKeyNew.ini`, `GBA.ini`, `FreeLookController.ini` and
    `Hotkeys.ini`, every section;
  - `WiimoteNew.ini`, with Source 0 as before.

  Every control that Dolphin 2609 binds by default is written with an empty
  expression; a Device line alone is not relied on. The list is in
  `dolphin_controls.json`:
  - 25 GC pad, 85 GC keyboard, 10 GBA and 20 free-look controls, from the
    defaults Dolphin 2609 saved on macOS (HWI-075) and Windows (this ticket);
  - all 144 hotkeys of `HotkeyManager.cpp` at Dolphin 2609 (`f84df020`).

  Dolphin's `InputConfig::LoadConfig` at that revision loads an absent ini by
  calling `LoadDefaults`, which binds the keyboard. An ini that has sections
  loads every unlisted control as empty, so the files alone are sufficient
  even for controls that are not listed.
- **Dolphin.ini, both hosts:**
  - `BackgroundInput = False`;
  - `PauseOnFocusLost = False` (in Core and Interface);
  - `WiiKeyboard`, `RenderToMain`, `Fullscreen` and `KeepWindowOnTop` off;
  - no update track.

  On macOS, `[SDL_Hints] SDL_JOYSTICK_MFI = 0` is added.
- **Launch environment, macOS.** Three variables are added to Dolphin's
  inherited environment:
  - `QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM=1` and
    `QT_MAC_SET_RAISE_PROCESS=0` stop Qt activating the process when it
    finishes launching or raises a window;
  - `SDL_JOYSTICK_MFI=0` stops SDL from opening a GameController HID client.

  Windows keeps its launch unchanged (STARTUPINFO `SW_HIDE`, measured below).
- **Focus guard, both hosts.** A helper thread inside the runner samples
  every 0.25 s while each launch runs.
  - On macOS it reads the frontmost application with `lsappinfo`, and this
    run's hidden and active state with `NSRunningApplication` through ctypes.
    It needs no helper binary and no permission.
  - On Windows it reads `GetForegroundWindow`, `GetGUIThreadInfo` and this
    run's visible top-level windows.
  - On macOS it hides this run's Dolphin as soon as it registers.
  - On either host it stops this run's Dolphin if it is ever frontmost or
    active, or holds keyboard focus. The root is stopped through the runner's
    own process handle. A descendant is stopped only through the HWI-073
    `kill_owned`, which refuses any PID outside the runner's tree.
  - Other applications and other lanes' Dolphins are only counted.
  - The runner keeps its direct child, its real exit status and its
    PID-scoped teardown check.
  - If the guard cannot start on its host, the runner refuses to launch
    (exit 2) rather than launching unguarded.
- **Audio.** Off by default on macOS, unchanged (on) on Windows.
  `--no-audio` and `--audio` override the default.
  - The lanes run on Macs that are in use (calls, music), and HWI-075 once
    ran a launch with audio on during a call.
  - The guest's DSP is emulated the same either way; only the host output
    backend changes. Guest bytes are identical (below).
  - Titan is the build host, with no one at it during this work (user input
    idle for more than 8 hours), so its default was left alone.
- **Records.**
  - Each run records `focus_guard`: samples, own frontmost/active/focus
    counts, hide requests, other Dolphins seen frontmost, query errors, and
    any stop.
  - Each run also records `input_binding_audit`, which reads Dolphin's own
    saved input configs back after the launch.
  - The batch records `focus_safety`: the added environment variables and
    settings, the input files, the controls file hash and the guard method.
  - A guard stop is host outcome `focus_guard_stop`, a guest failure
    `focus_guard_stop_before_natural_exit`, and it aborts the batch.
  - Existing fields keep their meaning; the launch shape is unchanged.
- **Comparison mode.** `--no-focus-safe` writes the earlier profile byte for
  byte, with no environment and no guard. `--focus-guard observe` samples
  without acting.

## Method

- **Guests**, each with two cold launches from a new stock-limit profile and
  an authored DTM:
  - `gx_materials.dol` `05263aee…05c9` (source `6d898c7c`), the HWI-075
    build, with `--efb-access`;
  - `probe.dol` `0e19586c…306f`, from the same build;
  - `gx_materials.dol` `194c63ae…1e19` (source `4a4aa208`), the HWI-073
    build, on Research.
- **Dolphin 2609.** Windows `Dolphin.exe` `1ed5e3f6…0dac`; macOS
  `b3c369bc…3ccf`.
- **Locks.** Every batch held one slot of the host's live counted-slot lock
  under its own owner (`hwi-077-a`, `hwi-077-r1`, `hwi-077-r2`,
  `hwi-077-c1`) and released it. The batches ran beside other agents' lanes
  with `--allow-concurrent`. No other lane's Dolphin was touched.
- **Independent sampling**, every 0.25 s for the whole batch, separate from
  the runner's guard:
  - macOS: HWI-075's `focuswatch` in read-only mode (no `--guard`, no
    `--hide`). It records the frontmost application and every Dolphin's
    active, hidden and on-screen window state.
  - Windows: a ctypes sampler. It records the foreground window's process,
    the keyboard-focus window's process, every Dolphin's visible top-level
    windows, and the session's idle time.
- **WindowServer audit (macOS).** `log show` over the batch window for
  WindowServer and launchservicesd only. Records are attributed to this
  batch's own PIDs through the ASN of each Dolphin process's
  `Process creation` record. The audit counts:
  - `SETFRONT pid=<own>`;
  - `keyboardFocus; Dolphin:<own ASN>`;
  - deferring-to-frontmost records;
  - `Connection added … pid:<own> process:Dolphin` (HID).
- **Command.** It was on battery (36% to 33%), so only these two short
  batches ran there; no a10.

## Results by host

| Host | Batch | Runner | Launches | Sampler: own frontmost / active / visible | Guard: own frontmost / active / focus | WindowServer: own SETFRONT / key focus / HID | Input audit |
|---|---|---|---|---|---|---|---|
| Research | gx_materials | `e7a2d3c3` | 2 | 0 / 0 / 0 of 152 | 0 / 0 / 0 of 96 | 0 / 0 / 0 | no input bound |
| Research | probe | `e7a2d3c3` | 2 | 0 / 0 / 0 of 81 | 0 / 0 / 0 of 26 | 0 / 0 / 0 | no input bound |
| Research | gx_materials (HWI-073 DOL) | `e7a2d3c3` | 2 | 0 / 0 / 0 of 152 | 0 / 0 / 0 of 95 | 0 / 0 / 0 | no input bound |
| Research | concurrent lane A, gx_materials | `283719a6` | 2 | 0 / 0 / 0 of 150 | 0 / 0 / 0 of 95 | 0 / 0 / 0 | no input bound |
| Research | concurrent lane B, HWI-073 DOL | `283719a6` | 2 | 0 / 0 / 0 of 152 | 0 / 0 / 0 of 96 | 0 / 0 / 0 | no input bound |
| Command | gx_materials | `283719a6` | 2 | 0 / 0 / 0 of 148 | 0 / 0 / 0 of 97 | 0 / 0 / 0 | no input bound |
| Command | probe | `283719a6` | 2 | 0 / 0 / 0 of 78 | 0 / 0 / 0 of 28 | 0 / 0 / 0 | no input bound |
| Titan | gx_materials | `a8818e1d` | 2 | 0 foreground / 0 focus / 0 visible of 4,508 | 0 / – / 0 of 137 | – | no input bound |
| Titan | probe | `a8818e1d` | 2 | 0 / 0 / 0 of 189 | 0 / – / 0 of 120 | – | no input bound |
| Titan | A/B: probe ×2, gx_materials | `283719a6` | 6 | 0 / 0 / 0 | 0 / – / 0 | – | no input bound |

Notes:

- **Sampler counts** cover the whole batch: lock wait, profile setup and
  settle. The Titan gx_materials batch includes 17 minutes of lock queueing.
- **Hiding on macOS.** The independent sampler saw this run's Dolphin
  unhidden in 1–4 samples per batch, about one per launch: the moment before
  the guard's first hide. It never saw a Dolphin window of this run on
  screen.

  AppKit answered NO to every hide request (`hide_unacknowledged`), yet
  hid the application before the next sample. HWI-075's watcher saw the same
  answers. The guard therefore re-requests whenever the application is still
  unhidden, and the record counts the effect (`own_unhidden`), not the
  answer.
- **Research's frontmost application** was another agent's standard-runner
  Dolphin in every sample of every batch: 687 sampler samples, and 398 guard
  samples counted as `other_dolphin_frontmost`. Every Dolphin keyboard-focus
  record in the Research windows names that process's ASN, not ours. There
  were 28; the two concurrent lanes' windows overlap, so some are counted
  twice.

  The guard counted that foreign process and never acted on it: it kept
  running through all five batches. The problem this ticket fixes will
  persist on Research until the other lanes use this runner.
- **Command's frontmost application** was a user application in all 226
  samples. The 16 keyboard-focus records in its windows all name other
  applications.
- **Titan.** The Windows foreground stayed with the desktop application that
  launched the runner (`claude.exe`) in every sample. That is the case where
  Windows lets a child process take the foreground, and Dolphin still never
  did.

## Windows (Titan) measurement

Measured first, with the unmodified runner (`bae56603`), before any change:

| Batch | Samples | Own Dolphin samples | Foreground | Keyboard focus | Visible windows |
|---|---|---|---|---|---|
| gx_materials | 613 | 117 | 0 | 0 | 0 |
| probe | 110 | 52 | 0 | 0 | 0 |

The runner's existing STARTUPINFO `SW_HIDE` already keeps all six of
Dolphin's top-level windows hidden. A hidden window is never activated, so
no further no-activate placement is needed (for example `SW_SHOWNOACTIVATE`
or `SW_MINIMIZE`). Windows keeps that launch.

The Windows gap was input: the default keyboard mappings above. With the
input changes, Windows gains the unbound profile, `BackgroundInput = False`
and the same guard, which stops Dolphin if it ever reaches the foreground or
keyboard focus. No Windows setting was changed.

## Guest identity

Every launch of each guest produced exactly one set of bytes, on every host,
with and without the changes:

| Guest, launch | Guest log SHA-256 | Counter or sentinel SHA-256 | Batches |
|---|---|---|---|
| gx_materials `05263aee`, 1 | `52997d07…` | `899715aa…` | 7: Titan old and new, Titan A/B old and new, Research ×2, Command |
| gx_materials `05263aee`, 2 | `10c92872…` | `773b5336…` | same |
| probe `0e19586c`, 1 | `13ef4b3a…` | `b8be899f…` | 8: Titan old and new, Titan A/B old ×2 and new ×2, Research, Command |
| probe `0e19586c`, 2 | `d452ab15…` | `09d554bd…` | same |
| gx_materials `194c63ae`, 1 | `b66f7922…` | `899715aa…` | 2: Research ×2; equal to HWI-073's old-runner output on Titan and Research |
| gx_materials `194c63ae`, 2 | `2d5b5cbe…` | `773b5336…` | same |

These bytes include the guest's own microsecond timings. The `05263aee` and
`0e19586c` outputs also match HWI-075's wrapper runs on Research and Command.

Removing host input therefore changes nothing for the guest: the DTM
supplies GC port 1 either way. Turning audio output off changes nothing
either: Titan ran with audio on, the Macs with audio off, and the bytes
match.

Every guest and persistence check passed. Every run exited 0. Every
`teardown_check` was `own_process_tree_exited`.

## Concurrent lanes

The two Research lanes ran at the same time in slots 2 and 1 of the live
3-slot lock, beside a third lane belonging to another agent (`lanes_max: 3`).
Each lane:

- recorded `host_shared`;
- saw two other Dolphins at most;
- tore down only its own tree;
- produced the bytes above.

Neither lane's guard stopped anything. Each counted the other agent's
frontmost Dolphin about 47 times per launch and left it running.

## Timing

Titan, old runner (`bae56603`) and focus-safe runner (`283719a6`)
interleaved in one sequence. Another agent's lane ran throughout, so every
launch saw one other Dolphin. These are shared-host times:

| Guest | Runner | Launches | Mean (s) | Range (s) |
|---|---|---|---|---|
| probe | old | 4 | 7.77 | 7.11–8.44 |
| probe | focus-safe | 4 | 7.07 | 5.78–9.04 |
| gx_materials | old | 2 | 23.25 | 21.61–24.89 |
| gx_materials | focus-safe | 2 | 20.69 | 18.25–23.14 |

No overhead from the guard is visible above the shared-host noise. The
sequence was stopped after six of its eight batches, because the lock queue
behind other agents' long runs made the remaining pair wait too long.

The first Titan focus-safe batches took longer (probe 13.6 and 17.8 s, against
6.5 and 7.0 s for the earlier old-runner batch). They ran at a different time
under heavier load from other lanes. The interleaved sequence above is the
comparison.

On Research the focus-safe runner's gx_materials launches took 11.8–12.3 s.
HWI-073's exclusive old-runner launches of the same build took 12.08–12.39 s.
Under the honest-timing rule these shared-host times are only an indication,
not an exclusive measurement.

## Guard behaviour and its tests

`test_run_dolphin_focus.py` (25 tests) mocks the focus queries:

- **Profile generation.** No device is bound anywhere, and every default
  control is empty on both hosts. The legacy profile's hashes are unchanged.
  The audit flags Dolphin's own defaults.
- **Environment.** The exact added variables, and none on Windows or with
  `--no-focus-safe`.
- **Guard.**
  - A quiet run is never acted on.
  - Its own root frontmost is stopped through the handle; another lane's
    frontmost Dolphin is counted, never stopped.
  - A descendant with focus is stopped by explicit PID only, and a foreign PID
    is refused.
  - An unhidden Dolphin is hidden, and an active one is stopped.
  - Observe mode never acts. Query errors are counted.
  - The thread samples until finished.
- **main().** The macOS launch carries the environment. Windows adds none.
  The legacy launch has no guard and the old files. A guard stop fails the
  run and skips later launches. An unavailable guard refuses to launch.
- **Real probes.** A read-only smoke test runs on each host: `lsappinfo` and
  AppKit on Research, user32 on Titan.

The live stop path was deliberately not triggered on a host. Triggering it
means letting Dolphin take the foreground from the user. HWI-075's incident
on Command already exercised it: a watcher with the same rule stopped a
clicked Dolphin 345 ms after the front switch.

## Checks

- Unit tests under `tools/wii`: 576 pass with Python 3.14 and 3.11 on
  Windows. `test_run_dolphin_focus`, `test_run_dolphin` and
  `test_run_dolphin_lanes` also pass with Python 3.12 on Research and 3.14 on
  Command.
- Build-only package: pending (built from a clean tree at this record's commit).

## Limits

- **Dock and Cmd-Tab (macOS).** Dolphin is still a regular application with
  a Dock icon and a Cmd-Tab entry. A user who deliberately switches to it
  gets it stopped within one sample (0.25 s), and that batch fails. That is
  the intended trade.
- **First moments of a launch (macOS).** Before Dolphin registers with
  LaunchServices nothing can hide it. Up to one sample interval can pass
  between registration and the hide. No window was seen on screen in that
  gap.
- **Keyboard focus on macOS** is inferred from the frontmost application,
  backed by the WindowServer log. There is no permission-free per-window
  focus query.
- **Long runs.** a10-length runs were not repeated here. HWI-075 ran a10 on
  Command with the same settings, and its output was byte-identical.
- **Other lanes.** Research stays exposed until the other agents' lanes run
  this runner.

Physical Wii: untested.
