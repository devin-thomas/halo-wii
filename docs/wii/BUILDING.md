# Building the Wii target

## Implemented probe

The asset-free probe is implemented. It does not load Halo assets or run gameplay.
Use the native root of your existing official devkitPro installation:

```powershell
python configure.py --wii --wii-devkitpro C:/dev/resources/toolchains/devkitpro
ninja wii_probe
```

`--wii` generates only Wii rules, without desktop SDL/LLVM setup. Without it,
the existing native configuration behavior is preserved. `DEVKITPRO` can supply
the root when invoking from the official shell. `--wii-probe-frames 300` selects
a bounded diagnostic run; the default 0 waits for PAD Start, Remote Home or a
system event. Do not use this timer as a gameplay simulation qualification.

Outputs are `build/wii/probe.elf`, `probe.dol`, `probe.map` and `build-info.json`.
The manifest records input hashes, compiler identity, SDK fingerprint, build ID
and artifact hashes. The same ID is embedded in the probe. SDK headers, tools,
libraries, startup files and Git identity trigger reconfiguration. Generated
files and machine paths stay outside Git. Debug info and linker maps name the
checkout as `.` and the devkitPro root as `/opt/devkitpro` (`path_prefix_map`).
[CI](CI.md) describes the build-only workflow and the allowlisted artifact set.

The native pipeline uses official Wii machine flags and libogc libraries:

```text
configure.py -> Ninja -> devkitPPC compile/link -> probe.elf -> elf2dol -> probe.dol
```

## Standalone scalar engine diagnostic

The asset-free scalar subset has its own host runner and Wii executable:

```powershell
python tools/wii/run_memory_subset.py --cc C:/msys64/ucrt64/bin/gcc.exe
python tools/wii/run_memory_subset.py --cc C:/dev/resources/toolchains/devkitpro/devkitPPC/bin/powerpc-eabi-gcc.exe --wii-devkitpro C:/dev/resources/toolchains/devkitpro --output build/wii-memory-subset
```

Replace the compiler and SDK paths with your installation. Outputs include
`memory_subset.elf`, `memory_subset.dol`, the map and `subset-info.json`.
The runner checks SDK/compiler identity, clears child GCC search overrides,
records input/SDK/artifact hashes and validates ELF/DOL structure. The Wii
executable writes `sd:/halo-wii-memory/subset.log` and returns its check result.
The [measured execution](evidence/2026-10-07-scalar-wii.md) reports 16 wire-byte
failures out of 128 checks; a successful build is not compatibility acceptance.

Add `--candidate` with a separate output directory to link the original
reference alongside an explicit wire-order diagnostic adapter. Use
`--output build/wii-memory-candidate` for Wii and
`--output .local/wii-memory-candidate-host` for host execution. This mode writes
`sd:/halo-wii-memory/compare.log`; its outer result covers candidate checks,
with reference results recorded separately. The
[candidate comparison](evidence/2026-10-07-scalar-candidate.md) passes 336
candidate checks on host/PPC while retaining the original PPC failures.

Add `--packets` to compare actual packet dispatch and string helpers with the
original scalar services and the opt-in adapter. Use separate outputs
`--output .local/wii-packet-host` and `--output build/wii-packets` with the same
host/Wii compiler arguments. This writes `sd:/halo-wii-memory/packets.log`;
the outer result includes both implementations. The host currently exits 1
and saves a complete manifest for the preserved version failures. See the
[packet comparison](evidence/2026-10-07-packet-dispatch.md) for per-unit warning
exceptions, exact failures and qualification limits.

Add `--packet-policy` instead of `--packets` to include a third isolated
excluded-field decoder and its version edges. Outputs should be separate:
`--output .local/wii-packet-policy-host` and `--output build/wii-packet-policy`.
It writes `sd:/halo-wii-memory/version.log`. The aggregate includes the two
preserved failing sections; the third passes 4,416 host/PPC predicates in the
[policy comparison](evidence/2026-10-07-packet-version-policy.md). This flag
does not integrate the policy into the engine.

Add `--packet-verifier` to execute the earlier scalar, packet and placeholder
sections alongside a standalone bounded flat-schema verifier. Use
`--output .local/wii-packet-verifier-host` or `--output build/wii-packet-verifier`.
It writes `sd:/halo-wii-memory/verifier.log`. The
[verifier comparison](evidence/2026-10-07-packet-verifier.md) passes 73 cases /
383 host/PPC predicates, preserving previous failed sections in the aggregate.
Arrays reject; runtime-version layouts and production integration remain open.

Add `--packet-arrays` to include the isolated recursive schema snapshot and
paired array codecs. Use `--output .local/wii-packet-arrays-host` or
`--output build/wii-packet-arrays`; Wii writes `sd:/halo-wii-memory/arrays.log`.
The [array comparison](evidence/2026-10-07-packet-arrays.md) passes 342,620
host/PPC predicates while preserving all earlier sections. Stable all-version
native reserves are a separate explicit policy; this flag does not integrate
any verifier or codec into production.

Add `--packet-groups` for the preserved actual group bodies and isolated bounded
wrapper, retaining all prior sections. Use separate outputs
`.local/wii-packet-groups-host` / `build/wii-packet-groups`; Wii writes
`sd:/halo-wii-memory/groups.log`. The
[group comparison](evidence/2026-10-07-packet-groups.md) distinguishes candidate
agreement from original plain-char differences and records two natural exit-0
observations for this DOL. Actual caller integration remains open.

Add `--packet-callers` for the three preserved header bodies, controlled actual
union-operation excerpts and bounded numeric-size/identity/framing bridge. Use
`.local/wii-packet-callers-host` / `build/wii-packet-callers`; Wii writes
`sd:/halo-wii-memory/callers.log`. The
[caller comparison](evidence/2026-10-07-packet-callers.md) passes 4886 host/PPC
checks and records actual endian-dependent reference values. Full caller,
native struct, socket and peer integration remain open.

Add `--packet-native-abi` to measure actual owner layouts/catalogs with guarded
identity/size binding while retaining all prior sections. Use separate outputs
`.local/wii-native-packet-abi-host` / `build/wii-native-packet-abi`; Wii writes
`sd:/halo-wii-memory/nativeabi.log`. The
[native diagnostic](evidence/2026-10-07-native-packet-abi.md) has zero predicate
failures but explicitly BLOCKED compatibility qualification. Only nine new
layout/catalog units use short-wchar; a separate default-width unit and all
prior/production flags remain unchanged. No actual engine cache/startup runs.

## Installation and provenance

Use the [official installer](https://github.com/devkitPro/installer) and [official Wii examples](https://github.com/devkitPro/wii-examples). Verify the current Windows installation instructions and Wii development package group when installing. Record package/compiler versions, Python/Ninja versions, source commit, environment variables and the exact successful commands. Do not mix an unrelated MSYS installation with the devkitPro shell without proving path compatibility.

Do not copy ancient SDK archives, proprietary Nintendo headers, unknown patched IOS bundles, or desktop build flags. Pin package versions after a known-good probe builds. A reproducibility manifest must describe the actual installed environment, not just say 'latest'.

The implemented read-only inventory command is:

```powershell
python tools/wii/check_toolchain.py --devkitpro C:\devkitPro
```

Replace the path with your actual native installation root. Missing tools return
nonzero with checked paths. A successful inventory still needs an unmodified
official example build before the project's probe. The current official Make
template is `templates/makefile/application` in the Wii examples repository;
locate its installed copy rather than assuming the older `templates/application` path.

## Future game and packaging targets

`ninja wii` and `ninja wii_package` remain **future and unimplemented**.
Their proposed artifacts are `build/wii/halo.elf`, `build/wii/halo.dol` and a
reviewed `build/wii/package/apps/halo-wii/` staging tree. No package currently
claims gameplay support. Packages must exclude game assets, saves and local
configuration.

## Required checks

Build the probe cleanly, then incrementally, including from a checkout path containing spaces. A missing compiler, unsupported package version or missing input must fail with an actionable message and nonzero exit. Verify ELF machine/endianness, DOL conversion, symbols and package contents. Build the reference desktop target separately to detect accidental upstream breakage.

Memory viability is a separate gate. An ELF that links does not prove the game fits Wii memory. Test encoded Xbox addresses, endian-safe assets/wire records and measured usable arenas before integrating a full map. See [compatibility](COMPATIBILITY.md).

## Running after compilation

Read [RUNNING.md](RUNNING.md). The optional [PowerShell sender](../../tools/wii/Send-WiiBuild.ps1) sends an existing executable only. It cannot manufacture a build target or validate gameplay.
