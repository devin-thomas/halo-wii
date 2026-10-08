# Selected campaign material streaming diagnostic

Clean source `b6f589916ece139183fde2928a18977346200132` passes the owned host diagnostic, all strict host/PPC
build commands and the authored fixtures. The emulated PPC guest finishes result zero; Dolphin exits naturally with host `0xc0000409` after 59.866 seconds, without timeout or kill.
[Numeric evidence](2026-10-07-material-stream.json) retains source, build, artifact and record
identities without asset bytes, names or private paths. This extends the
[material API](2026-10-07-material-api.md), [tag identity](2026-10-07-campaign-tag-identity.md)
and [campaign BSP metadata](2026-10-07-campaign-bsp-inspection.md) gates.

## Bounded data and publication

The selected snapshots contain 16,995,988 tag bytes / 3,647 instances and a
1,288,192-byte BSP, with 34 vertex and 34 index descriptors. The BSP streams
directly into its encoded offset 21,780,480 inside the unchanged 22 MiB tag
reservation; the 4,784,492-byte gap stays unreadable and its canary is checked.
There is no extra full BSP staging or serialization slot. This gate reuses
previously derived bytes and does not extract another map.

Both lightmaps and all 34 materials are projected or rejected atomically.
Full shader datum, primary group, shader ancestry and bounded tag-window NUL
names are checked. Material ranges are signed/in bounds without an ordering
requirement. Hardware resource membership and type-derived payload extents are
checked; NULL does not prove a resource extent, and empty counts preserve words
without following payloads. Environment vertices total 14,788; lightmap vertices
are zero. Vertex contents, triangles, shader bodies and float arithmetic remain
unqualified. Source limits remain 128 lightmaps, 2,048 materials per lightmap and
64,000 vertices per material; the selected 34 is not a hard cap.

Explicit LE projection keeps float bits and unknown bytes. Inspection
serialization concatenates the opaque 648-byte root, complete 64-byte lightmap
table and 8,704 material bytes. All 9,416 bytes match CRC `7f45c2f4` and full
SHA-256 `e58ffaeb6e7e82553d1d770a6e21e585d0246f3a0febb3dbba8ec3169e614022`.
Encoded addresses remain unchanged; this is not a relocated cache or durable save.
CRC/SHA establish byte identity, not the canonical Xbox cache checksum.

Views check copied arena/BSP parent epochs before material control/workspace,
then the material epoch. Failed publication retains the prior control/view and
published half; scratch may change. Sixteen parent cycles execute 16 BSP binds
and 32 material loads, separately covering child unload/rebind, parent release,
parent rebind and overwritten released workspace. Sources and controls require
truthful disjoint buffers, immutable pins and one caller.

## Builds, checks and memory

Host build `bd6484638e89fc44` and PPC build
`6e98590c95a2aa94` each compile 14 strict C11 units and finish
16 commands with zero exits/diagnostics. The generated SHA unit copies the
existing [SHA section](../../../port/linux/src/p2p_crypto.c) unchanged, with
declaration and width checks. Full source/extracted/generated hashes seed build
identity. Both inputs are SHA-checked on the first parent cycle; selected output
is SHA-checked on every material load. Numeric headers contain no asset arrays.

Authored checks pass address 1,916 / 8,437, arena 1,407 / 2,981, BSP 75 / 5,432,
material 64 / 48,752 and SHA eight vectors / 42 checks. Arena counts depend on
alignment. Host tmpfile IO passes 120 / 1,962; this fixture does not run on PPC.
Twenty-eight runner checks cover numeric/identity/window guards and a mocked
driver; those replaced compiler/link/runtime calls and are not build evidence.

Twelve 64-byte-aligned slots retain 22 MiB tags, 20 MiB state, 4 MiB sound,
65,535 index records of 32 bytes, two 64 KiB IO buffers and a provisional 2 MiB
reserve. Native material workspace is 8,832 bytes; selected serialization 9,416.
BSP/material controls are separately charged; caller view sizeof is not a full
stack charge. Conservative widget workspace/serialization remain charged without
campaign widget execution. State/sound are canary placeholders, not engine pools.
Numeric plan lines in the evidence report actual placement/alignment charges;
alternate placement charges 4,096 bytes against the original 54,368,224-byte span.
Three guarded owned intervals reject a request despite 54,237,152 aggregate bytes
(largest 18,122,720); this is not measured system allocator fragmentation.
Textures, initialized pools, call-chain stack, allocator overhead and total system
peak remain unqualified. Heap counters and restored bounds do not prove complete
heap-state restoration.

| Measured charged memory | Host | Emulated PPC |
|---|---:|---:|
| Required including provisional reserve | 52,582,744 | 52,582,096 |
| Maximum original arena charge at alternate placement | 52,586,840 | 52,586,192 |
| Minimum remaining from 54,368,224 bytes | 1,781,384 | 1,782,032 |

PPC MEM2 bounds restore exactly to `90002000..933db7e0`. Heap in-use 783,272,
arena 851,616 and free 68,344 match before/released observations. MEM1 after SD
setup is `8017a000..81800000` (23,617,536 bytes). These matching counters and
bounds are scoped observations, not a full allocator state or peak proof.


## Retained failures and open gates

The first host/PPC preflight each fails the eighth compile under Werror because
BSP parent rejection counters are set but not reported. Reporting/asserting them
fixes the error; no warning is suppressed. Exact failed argv, stdout/stderr,
generated files and full source snapshots remain private. Independent review also
finds a missing generated SHA declaration check and weak material-mode BSP
preflight. Including the header and reusing bounded BSP inspection fixes those
source-review findings; they are distinct from the executed compiler failure.
An earlier source review also finds sticky CRC status reused before a valid
reload; resetting status fixes it before the executed first preflight. This
review finding has no recorded runtime failure and is retained separately.

Independent build review passes 923 checks with zero unresolved findings:
clean source/build/artifact identities, ELF native sizes, per-function stack
records and charged slots agree. This read-only review adds no owned-byte read,
compiler/planner run, runtime launch or SD access. Its initial SHA comparison
normalized CRLF bytes incorrectly; the corrected exact raw-byte comparison
passes. The initial review record remains retained, with no product change.
Saved first-run runtime consistency review adds 323 checks with zero findings,
verifying retained metadata, guest reports and build/hash relationships. It
does not include the later repeat, launch a target or independently reread live
FAT/settings/assets. Preservation is checked for saved-record consistency.

The immutable DOL SHA-256 is `0a646518633f6678770112fc4bd5762fb2910908298113940ebd4d029014ca99`; guest LF report SHA-256
is `536f48f2677f1779402cf5d5e86fd4ccece38e94a191d009fb22309a4327f3b2`. One stock Dolphin 2609/D3D launch completes
the guest then faults naturally with `0xc0000409` after 59.866 seconds. The
180-second deadline does not expire; no forced stop occurs. All 19 prior SD
files, both private inputs and settings remain unchanged; only one new log is
added. Separate exclusive staging adds the two inputs after preserving 17 prior
files. A serialized independent repeat produces the identical 5,925-byte LF
guest report and naturally faults with `0xc0000409` after 59.6373275 seconds,
without forced stop. Its retained result SHA-256 is
`b2666eedc0f4790fcb6b1932b6bef8715a5c283995145664027f65605015163f`; prior files
and settings remain preserved, with the expected diagnostic-log append.
These are two separate scoped fault observations, without a general recovery claim.

The runtime observation is scoped to this DOL and stock profile.
Earlier exit faults, scoped clean exits and ABI qualification gaps remain
separate; there is no general host recovery or physical Wii proof. HWI-005/006
remain BLOCKED; HWI-007/008 remain TODO. Full engine memory strategy, shader/GX
registration, geometry, collision, gameplay and hardware acceptance remain open.
