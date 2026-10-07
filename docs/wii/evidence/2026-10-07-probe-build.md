# Native probe compile qualification

Tested source: `3bca2e612cc118df5c5aa354ec7deb61aeb31ae6`, clean input tree. Build ID:
`0f01e11c08c44932`. See the adjacent JSON manifest for source/SDK/artifact hashes.
The probe's build-ID string was checked in the binary; screen verification is
part of the separate Dolphin run.

Commands executed on Windows with the existing shared official devkitPro root:

```powershell
python configure.py --wii --wii-devkitpro C:/dev/resources/toolchains/devkitpro --wii-probe-frames 300
ninja -t clean wii_probe
ninja wii_probe
ninja wii_probe
python tools/wii/test_check_toolchain.py
python -m pytest tools/test_wii_build.py tools/test_linux_port.py -q -k "wii or strip_cplusplus or scan_finds or render_skips or xdk_headers_use or game_sources_leave or menus_are or menu_settings_exist" --basetemp=.local/pytest-final-hwi004 --tb=short
```

Clean build passed; incremental build had no work. A fresh local Git clone at
`.local/source with spaces/OpenCE` also configured/built/incrementally passed,
using the same SDK installation. Committing the source triggered automatic Ninja
reconfiguration and changed the manifest from dirty to the committed identity.
The incorrect-root check failed early with exit 2 and checked-path diagnostics.
ELF32/big-endian/PowerPC executable segments and DOL ranges were validated.
Default controller-exit mode (0 frames) also compiled during bring-up.

Three preflight rejection tests and 15 selected build/native-source regression
tests passed (8 unrelated tests deselected). Initial module-style invocation of
the preflight test could not import its direct-script dependency; the documented
direct invocation above passed. No full desktop compile was attempted: verified
LLVM/MSVC/SDK and owned data are still absent. Routing regression coverage is
not a desktop binary qualification.

This record proves compilation only. No Halo assets, gameplay, emulator runtime,
physical Wii, IR, co-op or networking are qualified here. Generated executables,
maps, raw logs and native configuration remain outside Git.
