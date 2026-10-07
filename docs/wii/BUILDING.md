# Building the Wii target

## Current versus planned commands

This file is a build contract, not a claim that Wii targets already exist. The audited upstream configure.py at `2b0327bc80ca38c90894cb56b19652cbe85733ff` generates desktop/Android targets only. Implement the Wii generator before running any command labelled **planned** below.

The intended native pipeline is:

```text
configure.py → Ninja → devkitPPC compilation/link → halo.elf → elf2dol → boot.dol
```

Use official devkitPro/devkitPPC, libogc and the Wii dependencies distributed through devkitPro. Start from the installed official Wii example/rules for flags, linker script, alignment and libraries. Windows is a first-class development host; WSL is not mandatory. Existing desktop reference builds have their own dependencies and do not replace the Wii toolchain.

## Installation and provenance

Use the [official installer](https://github.com/devkitPro/installer) and [official Wii examples](https://github.com/devkitPro/wii-examples). Verify the current Windows installation instructions and Wii development package group when installing. Record package/compiler versions, Python/Ninja versions, source commit, environment variables and the exact successful commands. Do not mix an unrelated MSYS installation with the devkitPro shell without proving path compatibility.

Do not copy ancient SDK archives, proprietary Nintendo headers, unknown patched IOS bundles, or desktop build flags. Pin package versions after a known-good probe builds. A reproducibility manifest must describe the actual installed environment, not just say 'latest'.

## Implemented-target contract (future)

The following interface is **proposed and absent from the audited baseline**:

```sh
python configure.py --wii
ninja wii_probe
ninja wii
ninja wii_package
```

The implementation must add these rules, their dependencies and clear help/error output before documenting them as usable. The generator must not require desktop-only dependencies merely to build the Wii probe. Wii compilation must not inherit x86 flags, desktop PGO profiles, Clang-only options, SDL3 or GLES dependencies by accident.

| Planned target | Artifact contract |
|---|---|
| `wii_probe` | `build/wii/probe.elf` and `build/wii/probe.dol`; asset-free diagnostic |
| `wii` | `build/wii/halo.elf` and `build/wii/halo.dol`; symbols retained in ELF |
| `wii_package` | Staging tree `build/wii/package/apps/halo-wii/` plus reviewed release archive |

These paths must match the implementation and tests; revise this contract with the generator if a justified path changes. A package includes authored metadata/icon when available, public documentation and a build manifest. It does not include game assets, saves or local configuration.

## Required checks

Build the probe cleanly, then incrementally, including from a checkout path containing spaces. A missing compiler, unsupported package version or missing input must fail with an actionable message and nonzero exit. Verify ELF machine/endianness, DOL conversion, symbols and package contents. Build the reference desktop target separately to detect accidental upstream breakage.

Memory viability is a separate gate. An ELF that links does not prove the game fits Wii memory. Test encoded Xbox addresses, endian-safe assets/wire records and measured usable arenas before integrating a full map. See [compatibility](COMPATIBILITY.md).

## Running after compilation

Read [RUNNING.md](RUNNING.md). The optional [PowerShell sender](../../tools/wii/Send-WiiBuild.ps1) sends an existing executable only. It cannot manufacture a build target or validate gameplay.
