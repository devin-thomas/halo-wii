# Actual packet-group boundary diagnostic

Clean source `7e07d084d30b7315733fbc09f72c3c3c1edc40df` adds an isolated
reference using the complete actual group translation unit and a separate
bounded wrapper around the existing paired array codec. All new predicates
pass: host 1688, PPC 1682, reported cases 271 each, failures 0 and aborted 0.
The case counter includes named decode vectors tested at eight offsets and
individual loop iterations; it is not a count of unique schemas.

Every earlier host and PPC report line matches its respective published
[array evidence](2026-10-07-packet-arrays.md), including original failures.
All candidate group report lines match across platforms. Original synthetic
types 128/255 differ deliberately in the measured reference: host plain char
is signed and rejects them; PPC plain char is unsigned and accepts them in a
256-entry null-definition table. The six additional host assertions retrieve
those two errors. This is an observed reference difference, not exact equality
of all group reports. Real network/key groups have only 35/2 types below 128.

The [source/caller audit](2026-10-07-packet-group-contract.md) covers all five
group bodies, 34 network decode sites and key-agreement wrappers. Production
sources, earlier fixtures, goldens, metadata policies and failed assertions
remain unchanged. No network session or actual message-struct integration is
established by these authored payloads and group-table shapes.

## Preserved reference and explicit wrapper policy

The reference generator prepends the diagnostic service header and appends a
read-only observer of the original shared error slot. The complete normalized
original source, structures, globals and five function bodies are retained.
An authored service validates the actual one-byte group descriptor, marks it
verified and leaves packet bytes unchanged. It does not qualify the general
byte-swap descriptor walker or its pointer-size assumptions.

The wrapper borrows live immutable plans produced by `packet_array_compile`
and a truthful accessible entry table. It never initializes production caches.
Type count 1..128 gives portable trailer types 0..127; byte types beyond that
are rejected. Class count is positive signed short; each entry and expected
class is 0..count-1. Decoded maxima are 0..32767, encoded maxima 1..32767,
avoiding the original unchecked long-to-short narrowing. Every nonnull plan
must fit the decoded maximum and retain the original conservative native
reserve plus trailer <= encoded maximum rule. Null-entry classes are also
validated, unlike the original initializer which skips them.

Encoding ignores the incoming size value and passes the smaller of actual
wire capacity and configured maximum to the paired payload encoder. It then
retains the original strict append rule: payload plus trailer must be strictly
below the configured maximum, while fitting actual writable capacity. Payload
failure retains partial output/offset; append failure retains the completed
payload and its size without writing a trailer. Per-call results replace only
the new wrapper's error state; the original shared slot remains independent.

Decode first checks nonnegative supplied length against actual accessible
wire capacity and validates class/type. It reads the final byte as the trailer,
then checks native/wire overlap including that trailer before payload IO or
changes to caller size/type/version. Forged pointers or false accessible extents
are outside the contract. Result/size/type/version and plan control storage must
be disjoint from data and one another.

Valid type/class removes one byte from the caller's length before payload
decode. Payload capacity/version/count/truncation failure keeps that decrement
and prior writes/version changes. Type is published only on success. A null
definition succeeds with type only and leaves native/version unchanged; encode
rejects it. Supplied length, post-trailer payload length and actually consumed
payload bytes are separate fields. Trailing payload bytes remain accepted and
unconsumed. Decode also retains the original absence of a configured encoded
maximum check on incoming length; the actual accessible capacity is enforced.
No global exact-consumption rule is introduced.

## Checks, commands and strict failures

```powershell
python tools/wii/run_memory_subset.py --cc C:/msys64/ucrt64/bin/gcc.exe --packet-groups --output .local/wii-packet-groups-host
python tools/wii/run_memory_subset.py --cc C:/dev/resources/toolchains/devkitpro/devkitPPC/bin/powerpc-eabi-gcc.exe --wii-devkitpro C:/dev/resources/toolchains/devkitpro --packet-groups --output build/wii-packet-groups
```

Group mode executes every previous section independently before aggregation.
Host aggregate exits 1 and guest result is 1 because earlier packet/scalar
failures remain. Previous scalar candidate 336, placeholder 4416, flat verifier
383 and arrays 342620 still pass. Runnable warnings are 15 host / 11 PPC.

Independent numeric payload/trailer goldens and complete 512-byte snapshots
cover offsets 0..7, every ordinary payload truncation and encode capacity,
strict append equality/slack, future version, signed lengths, class/type order,
null definitions, type 127 and high-byte rejection, table/max/class limits,
partial output, supplied/consumed separation, overlap and touching spans.
A trailer-only overlap case would be missed by the payload codec alone.
Different selected types borrow flat reserve 2 and array reserve 4; count-zero
wire `[1,0,2]`, insufficient native capacity and invalid counts preserve the
array policy through the group boundary. Full source/cache/entry/plan/node
snapshots verify candidate immutability before original initialization.

Safe original byte-schema cases measure actual initialization/cache reuse,
trailer append, decode/error ordering, trailing bytes, null definitions and
plain-char behavior. The observer proves initialization retains a pending
error, another group overwrites it, the getter clears it, and success clears
it. Every getter is guarded against an unexpected NULL even in collect mode.
Fatal empty getters, original negative lengths, invalid initialization and
null-definition encode are not executed as safe reference cases.

Six authored unit compilations (wrapper, service, fixture on host/PPC) pass
strict Werror with zero diagnostics and no warning exceptions. The unchanged
original group unit fails strict compilation: three sign-compare plus one
char-subscripts diagnostic on host, plus type-limits on PPC. Runnable exceptions
apply only to that reference unit; warnings and earlier strict failures remain
recorded. The first complete fixture failed for the original append function's
missing header declaration; a matching fixture-local prototype fixed it.

Coordinator independently reproduced the original group strict failures and
clean wrapper/service compilation at exact recorded hashes. Its earlier
fixture snapshot compiled cleanly; removing the local declaration in a separate
copy reproduced that condition, not the historical original fixture hash.
Neither fixture snapshot is the final runtime fixture. Final fixture SHA256
`891cbadede8575003a413ed00db8b95cfb95fb9b4202b49e10267dcc38e149df`
passed independent source review after guarded-getter and trailer-overlap fixes.

## Runtime identities and remaining gate

Build `85649dff01cb0aa0`; DOL SHA256
`0f2f3e945d91deb1ad056e99308408c57d731a10783e6296c6bd117d9965b8f0`.
Stock Dolphin 2609 executable SHA256 remains
`1ed5e3f63f08b672133dade2c570e87ea4fa7fac2f6a65a118cc300a18dc0dac`.
The same isolated profile and CLI D3D override were used. CPUThread, cheats,
overclock and RAM override remain disabled; no profile/settings edits were made.
Read-only SD `halo-wii-memory/groups.log` has matching BEGIN/END. Its normalized
UTF-8 LF report SHA256 is
`184eef900118bb1fc7483aa9e96b605bba054363e466674b8b2dcb02dd47529a`.

Two same-DOL runs completed with identical guest reports and natural host
exit 0 after 16.327s and 17.508s, without timeout/kill. This differs from the
earlier preserved `0xc0000409` runs. These two observations qualify only this
diagnostic DOL; they do not establish general shutdown recovery or close
HWI-005/006. No additional shutdown matrix was run.

Sanitized [host](2026-10-07-packet-groups-host.json),
[build](2026-10-07-packet-groups-build.json) and
[runtime](2026-10-07-packet-groups-runtime.json) records retain reports, hashes,
commands, strict diagnostics, reproduction scopes and both shutdown observations.

Next bounded gate: actual caller size-union numeric conversion, dispatch
identity/native capacity association, outer framing and initialization ordering.
The audited long/short union writes can update the high half on PPC instead
of numerically widening a produced size; this remains source-derived here.
Production migration, real structs/callers, peers, broader ABI, physical Wii
and gameplay remain open. No production, Dolphin fork, security, console or
dashboard changes were made by this worker.
