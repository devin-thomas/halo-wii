# Dolphin probe: guest checks observed, qualification blocked

Stock Dolphin 2609 from the [official download page](https://dolphin-emu.org/download/)
ran the asset-free probe in a newly created isolated local profile. Exact binary,
source/build IDs, settings and outcomes are in [the record](2026-10-07-dolphin-probe.json).
No owned game data, existing emulator NAND/profile, cheats, extra RAM or overclock
were used. Dolphin generated its standard 128 MiB emulated FAT32 SD image.

Verified guest observations: rendered build ID `0f01e11c08c44932` matches the
manifest; 640x480 output; controller 0 connected; timing/arena output; flushed
BEGIN/END records; sentinel persisted and incremented through new processes.
An authored standard DTM movie supplied neutral, button/analog/trigger activity
and Start through Dolphin's ordinary GameCube controller device. The native
probe recorded `connected=1 activity=1 exit=pad_start` at frame113. This tests
virtual controller input, not a physical controller or human input session.

Every batch run ended with Dolphin process exit `-1073740791` (`0xc0000409`).
Windows records identify ucrtbase.dll and fatal-app-exit subcode7. The guest
closed its log before that host failure; no clean Dolphin pass is claimed.
The exception code alone does not prove a stack overwrite. OGL/D3D, frame dump
on/off, disabling SDL DirectInput, child-process C locale, disabled Wii remotes,
and a separate stock2603a comparison did not resolve it. Non-batch GUI retained
an idle process after guest END; a bounded diagnostic stopped that owned process.

The standard CLI was verified with `--help`. Launch shape actually tested:

```powershell
$dolphinProbe = Start-Process -FilePath <isolated-Dolphin.exe> -ArgumentList @('--user',<new-profile>,'--batch','--video_backend','D3D','--exec',<probe.dol>) -WindowStyle Hidden -PassThru
$dolphinProbe.WaitForExit(20000)
$dolphinProbe.ExitCode
```

`--movie <authored-input.dtm>` was added for the input test. The local profile's
Dolphin.ini sets CPUThread/cheats/overclock/RAM overrides off, normal emulation
speed, SIDevice0=6 and the other three ports0, writable WiiSDCard on and folder
sync off. GFX.ini uses InternalResolution1. Raw profiles, SD image, movie, logs,
process metadata, screenshots and downloads remain outside Git.

An official2609-69 development comparison was not executed: automatic approval
review rejected its combined download-and-launch command as "blocked by policy"
without a more detailed reason. Next work is a supported host-teardown diagnosis
or a separately approved stock build comparison; do not weaken runtime acceptance.
Physical Wii and all Halo gameplay remain untested.
