# Native packet ABI and guarded catalog binding diagnostic

Clean source `9f4ca2259f2a9f795e9f1d96a90cf021da69b801` measures 58 actual native layouts and 127 direct
members from six source owners. Host and PPC each complete 420 reported cases /
3387 predicates with zero diagnostic failures and zero aborts. Compatibility
qualification is explicitly BLOCKED: seven known host gaps and nine PPC gaps
remain. Aggregate host exit1 / guest result1 preserve earlier failures and the
new blocked qualification; a zero diagnostic-failure count is not an ABI pass.

The [source contract](2026-10-07-native-packet-abi-contract.md) separates real
declarations, catalog order, default widths, raw records and declared peer
identity. The generator retains 89 hashed source snippets, including the actual
player_action sizeof and nested yaw/pitch assertions. Fourteen client-handler,
16 server-handler, 11 client-manager, 12 server-manager, three key and two player
layouts are compiled independently to preserve owner-local declarations.
All 35 network and two key catalog rows retain their actual definitions, field
arrays, flags, classes, order and group limits. No production initializer runs.

## Preserved qualification gaps

| Association | Measured/source-derived gap |
| --- | --- |
| Network enum IDs 8/9/10 | Enum begin_game/graceful_exit_pregame/pregame_keep_alive selects table pregame_keep_alive/begin_game/graceful_exit_pregame |
| Client game-start request ID 17 | Actual server receiver long4 versus sender short2 and catalog/schema2 |
| Three typed advertisement owners | XDK transport/map dependencies remain unsupported; opaque catalog layout is not typed owner proof |
| PPC default wchar | Default4 versus required selected 2; only nine new diagnostic layout/catalog units use -fshort-wchar |
| PPC plain-char identities | CHAR_MIN0 and char(-1)=255 versus host -128; typed NONE comparisons remain unqualified |

The first three rows account for seven common gaps (three identity, one receiver,
three unsupported owners). PPC adds two width/signedness gaps. Actual table,
enum and caller discrepancies are preserved. Original potentially unsafe
8/9/10 payload calls and ID 17 receiver interpretation are not executed.
Default wchar has its own unit without override; prior units and all production
flags remain unchanged. No signed-char override is added. Upper-bound and
VALID_INDEX guards remain relevant; this is not a blanket out-of-bounds claim.

## Bounded binding and representation checks

The isolated guard checks supplied enum type/name against actual row and
definition name before schema compilation, then requires actual owner sizeof,
declared definition size and compiled stable reserve to agree. Rejection leaves
the zero-initialized destination untouched; success publishes an owned immutable
schema snapshot in one assignment. Names/source are truthful, live and immutable
during binding. Group entries still borrow live immutable plan controls and
validate class/type/capacity independently. This is not engine dispatch or cache
initialization. A same-size/class row-name tamper rejects atomically before IO.

Handwritten numeric BE oracles exercise actual schemas for network types
0,1,3,4,6,12,15,20,25 and player aliases 13/21, plus both key types. Fixed offsets
and explicit shifts are independent of schema traversal. All offsets 0..7 use
full 4480-byte native/workspace/frame snapshots, preserve padding/unused reserve
and decode input mutation, and check exact sizes/consumption. Actual server
array count 128 gives native 4112 / payload 3854 / frame 3857; client count 4 gives
native 136 / payload 126 / frame 129. Counts 0,2,max, native-1/max+1, overmaximum wire
count and one-byte-short stable native capacities are checked with partial
effects preserved. These are inert object representations, not player gameplay.

All 37 original schemas compile read-only into separate plans. Every original
group/entry/definition/field snapshot remains intact. An owned bound plan still
encodes after copied mutable source fields and definition are changed and freed.
Destroying that binding leaves its control object live and zero; a group still
borrowing it rejects before IO. No dangling control is dereferenced, and this
does not qualify actual engine cache lifetime or concurrent mutation.

Raw advertisement and settings bytes remain raw. Matching size/offsets cannot
establish nested scalar, Unicode or float byte order for an actual peer. Source
declares packet version1, discovery2, native22 and session128; no deployed peer
capture or acceptance exists. Catalog authority, real raw-record wire contract,
typed signed semantics, cache/dispatch ownership, socket partial IO, shared-frame
reuse and startup ordering remain prerequisites for production migration.

## Reproduction and runtime identity

```powershell
python tools/wii/run_memory_subset.py --cc C:/msys64/ucrt64/bin/gcc.exe --packet-native-abi --output .local/wii-native-packet-abi-host
python tools/wii/run_memory_subset.py --cc C:/dev/resources/toolchains/devkitpro/devkitPPC/bin/powerpc-eabi-gcc.exe --wii-devkitpro C:/dev/resources/toolchains/devkitpro --packet-native-abi --output build/wii-native-packet-abi
```

Every earlier report line exactly matches its respective published
[caller baseline](2026-10-07-packet-callers.md). Original scalar/packet/group/
caller generated identities are unchanged. Native common report lines agree;
full native-section reports differ only in default width/plain-char observations, two extra PPC
gaps and summary gap count. All 24 strict new object compilations (20 generated,
four binding/fixture) pass without diagnostics. Runnable warnings stay 17 host /
13 PPC, with only prior original-unit exceptions. Independent final source
review verifies all 89 copied snippets and has zero unresolved findings;
independent raw-record review confirms prior reports, clean inputs, artifact and
guest identities. Final fixture SHA256
`b0ec9ef714599234da9a9b235c1d4ee7e68797b9dd66fc067f8c25f9ccf6ce92`.

Build `a1c968a4fdaaeeac`; DOL SHA256
`0a6384750641329fb062cf2e93ea96a9503811e8d4b786ade13a7ac4f5c5e047`; normalized UTF-8 LF guest SHA256
`8ffdabd80a3895a528c4a28153cae43e509c3fdb2da19f4a272a9b04a060412b`.
Stock Dolphin 2609 executable SHA256 remains
`1ed5e3f63f08b672133dade2c570e87ea4fa7fac2f6a65a118cc300a18dc0dac`.
Same isolated profile and D3D CLI override; no settings edits. Read-only SD
nativeabi.log has matching BEGIN/END/result1. One owned natural run exits host 0
after 17.344s, without timeout/kill. Earlier 0xc0000409 faults, including both
caller observations, and both group-DOL exit 0 observations remain preserved.
General shutdown recovery is unqualified; no new shutdown matrix ran.

Sanitized [host](2026-10-07-native-packet-abi-host.json),
[build](2026-10-07-native-packet-abi-build.json) and
[runtime](2026-10-07-native-packet-abi-runtime.json) records retain exact hashes,
reports, source excerpts, compile overrides and scope limits. HWI-005/006 remain
blocked. Full engine, peer sessions, physical Wii and gameplay remain untested.
The next independent data gate is read-only inventory and cache-version
validation of the available owned XISO using existing import tooling; game
bytes and raw intake records stay outside Git.
