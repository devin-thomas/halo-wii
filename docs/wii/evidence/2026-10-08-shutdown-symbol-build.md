# Isolated shutdown symbol-build attempt

The exact official Dolphin source build fails at Ninja step 510 of 2,029 on
`Source/PCH/pch.h:11`, which requires `_MSC_FULL_VER >= 195136252`.
Installed MSVC reports `194435228`. A separate unchanged-command retry confirms
the same C1189 compiler requirement. No Dolphin PE, PDB or map is qualified.
[Numeric evidence](2026-10-08-shutdown-symbol-build.json)

The earlier feasibility check covered CMake's weaker MSVC19.32/v143 minimum
and missed this stricter compile-time prerequisite. A successful configure
therefore did not establish compiler compatibility. The pinned
[PCH guard](https://github.com/dolphin-emu/dolphin/blob/f84df02055ab9610feec48e65648cac5a3c098fa/Source/PCH/pch.h#L9)
defines the actual required threshold; the earlier candidate record is retained
with this correction rather than replaced.

Exact revision `f84df02055ab9610feec48e65648cac5a3c098fa` and all 38 pinned
repositories were acquired and verified, with 35,773 inventory entries. Initial
acquisition stopped at an unreviewed nested prerequisite; that failure and the
subsequent authorized pinned continuation remain saved. The first configure
failed because its captured environment omitted host architecture and the
selected redistributable directory. Correcting that environment produced a
successful native configure with pinned x64 Qt6.8.3 and SDK26100. Its earlier
controller failure on lingering compiler helpers also remains a failure.

The final controller reused that generated tree without another configure.
It verified Windows AMD64/x86_64, all recorded source features, Release `/O2`,
`/Z7`, `/DEBUG`, `/MAP`, `/OPT:REF`, `/OPT:ICF`, C++latest and LTO OFF. Upstream
`CMAKE_CROSSCOMPILING=TRUE` was recorded without changing its toolchain.
The prerequisite budget retained 711 seconds already charged and 1,089 seconds
remaining. One serial `dolphin-emu` build had a 90-minute limit and resource
thresholds of 3GiB free RAM and 60GiB free C: space.

The actual build command returned MAIN1 after 115.925 seconds; the outer
controller returned1 after 242.858 seconds, including source qualification.
Because MAIN failed, the new exact-helper cleanup acceptance did not apply.
The remaining owned helper was forcibly cleaned up and the job drained to zero.
The independent retry also returned MAIN1 and drained its owned job to zero.
An ancillary ScmRevGen `^master` revision diagnostic is retained without a
separate causal or generated-version claim.

Saved-record review passes 3,910 predicates with zero inconsistencies, covering
646 command/raw-log pairs and 3,354,243 log bytes. Minimum sampled availability
was 11,993,395,200 RAM bytes and 181,858,324,480 C: bytes. These samples do not
measure a successful linker's peak. Earlier raw failures and initial reviewer
oracle errors remain retained. Postbuild source inventory, artifact hashes,
own PE RSDS and matching-PDB DIA validation were not reached.

No authored upstream source patch, feature cut or toolchain installation follows.
No emulator, SD, profile or owned-asset operation occurs in this build gate.
Rebuilt symbols never substitute for stock RVAs. HWI-005/006 remain BLOCKED,
HWI-007/008 TODO; source/thread ownership, engine, native geometry/GX, gameplay
and physical Wii acceptance remain open.
