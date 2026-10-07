# Bounded recursive array diagnostic

Source `5b7395e2e48d01e5d210b4c9ca0f9ea4e3cf2a92` implements an isolated
schema snapshot and paired encoder/decoder. Host and PPC each pass 306 case
executions / 342,620 array predicates, failures 0 and aborted 0. Every array report
line matches; all previous scalar, packet, placeholder and verifier report lines
also match their respective published host/PPC evidence. Original engine bodies,
generated packet units, fixtures, goldens and failed assertions remain intact.

The [caller/group audit](2026-10-07-packet-array-contract.md) establishes two real
array shapes: server native 4112/capacity 128 and client native 136/capacity 4, both
version 1 with zero gates, child stride 32 native/30 wire. Included legacy arrays
already skip child schemas, including real count-zero behavior. Legacy excluded
branches do not skip. Their verifier stores whole reserves on child END, and
included codecs depend on that placement. The new codec uses separate copied
metadata and paired traversal; none of that legacy metadata is changed or mixed.

## Explicit candidate policy

The compiler takes a truthful accessible schema bound 1..32767 and walks each
entry once with an explicit heap stack. It requires separate child and root END
markers, validates every child even when its parent is excluded, ignores cached
sizes/initialized state, and leaves the source definition/table untouched on
success and failure. An unused suffix after root END remains ignored. A shared
nonmutating helper reuses the earlier flat verifier's native extent arithmetic;
all existing verifier bodies remain unchanged.

Every field reserves its full latent extent at every runtime version. Arrays
reserve `2 + maximum_count * full_child_stride`; subtraction/division checks
precede multiplication/addition, with individual/total reserve bounded by 32767.
Definition size must equal that stable reserve. This differs deliberately from
the earlier flat diagnostic's own-version excluded size 0. The runtime requires
actual native capacity at least the full reserve before IO. Byte-range gates
and positive schema counts are diagnostic choices; no deployed nested or gated
array compatibility is established by real callers.

Plans own copied fields and child-skip indices. Runtime uses explicit heap
frames sized to measured depth, with no recursive C calls. Only non-END field
visits count toward the native-extent budget: each reserves at least one byte,
array headers at least two. Empty child schemas skip element iteration entirely.
END control steps can exceed that visit count and are handled separately.

Encode accepts NONE as own version and otherwise 0..definition version; a
version-zero definition has no prefix. Decode rejects future versions after
reading the prefix. Wire capacity/length must fit a nonnegative signed short;
native 32767 plus a version byte can overflow the wire bound and fails without
wrapping. Native/wire overlap rejects before IO with checked address arithmetic;
touching disjoint spans are allowed. Plan/result/optional-version control storage
must be separate from data objects and each other, and the plan must be immutable.

Included arrays validate signed counts before child traversal and skip children
at count 0. Valid counts are stored before decoding children; unused reserves
stay untouched. Invalid counts leave the current native field untouched after
consuming/converting its wire prefix, differing explicitly from the original
zero-on-invalid behavior. Count loads/stores use memcpy, including odd addresses.
Included scalar decode retains the prior candidate's in-place conversion;
truncation preserves prior writes/conversion and stops.

Excluded arrays emit/consume a raw one/two-byte count placeholder, accept nonzero
contents without interpreting them, zero the full reserve only after successful
read, and skip the entire child schema. Excluded flat fields reuse the existing
placeholder helper. Strings use checked bounded terminators and memcpy. Zero
spans and exhausted write cursors are handled before calling the older scalar
candidate's assertion. Successful decode continues to accept trailing bytes
without consuming them; no global exact-consumption or group rule is added.

## Execution and coverage

```powershell
python tools/wii/run_memory_subset.py --cc C:/msys64/ucrt64/bin/gcc.exe --packet-arrays --output .local/wii-packet-arrays-host
python tools/wii/run_memory_subset.py --cc C:/dev/resources/toolchains/devkitpro/devkitPPC/bin/powerpc-eabi-gcc.exe --wii-devkitpro C:/dev/resources/toolchains/devkitpro --packet-arrays --output build/wii-packet-arrays
```

Clean host aggregate exits 1 because earlier packet reference/candidate fail 16
each. PPC original scalar fails 16/128; its scalar candidate passes 336. Original
packet reference fails 432/1504, scalar-backed packet candidate fails 16/1504.
Placeholder decoder/edges pass 4416 and the prior verifier passes 383 in 73 cases.
New arrays pass 342620 in 306 case executions on both platforms, no aborts. Outer
guest result 1 preserves those failures; every section executes before aggregation.

Manually authored wire/native goldens and event timelines check every truncation
and encode capacity for the ordinary vectors at offsets 0..7, comparing complete
guarded objects. Coverage includes included counts 0/1/max, selector 255/256,
nonzero excluded placeholders, inclusive/maximum/own-version gates, nested
arrays/shorts/pads/strings/data, empty schemas, native capacities, signed negative
and excessive counts, version/length bounds, overlap and touching boundaries,
malformed child schemas, missing child/root END, exact/overflow reserve arithmetic,
stale source metadata and immutable plans. Both real server/client shapes test
counts 0/1/max against independent numeric goldens; these are authored shapes,
not network sessions or actual message-struct integration.

Representation-limit fixtures allocate 32767 schema fields: 16383 array headers
and 16384 ENDs, depth 16384, stable reserve 32766. Valid and one-byte-truncated IO
pass at every offset. Another array has 32765 one-byte children, reserve/wire 32767,
proving repeated END control steps do not exhaust the non-END budget. Full large
guarded-object/source snapshots accompany these limits. Captured validation
checks the complete 306/342620 totals; case executions include repeated offsets.

Six authored unit compilations pass host/PPC Werror without warning exceptions
or diagnostics. Runnable warnings remain 11 host / 6 PPC, preserving original
maybe-uninitialized/string/LLP64 diagnostics and exceptions. The initial fixture
strict compile caught adjacent-loop misleading indentation; it was fixed without
weakening flags. Independent review also corrected overlap, allocation cursor
reporting, resource cleanup and collect-mode guards; final source has no unresolved
findings. A host linker calloc fault-injection probe passes four cases / ten
assertions: compile allocation failures preserve source/plan, and runtime workspace
failure after the version prefix reports offset 1 and preserves prior IO. It is
host-only and introduces no production hooks or full-packet atomicity claim.
Coordinator independently strict-compiled a corrected earlier fixture revision;
its exact hash is retained separately from the final two failure-path guards.

## Identities and remaining gates

Build `d735d8e69153d744`; DOL SHA256
`810bbb550af451cfaf516f89fe82619e1585ba39de60b93a8a41142bc9a73316`.
Stock 2609 executable hash and isolated-profile/D3D settings remain unchanged.
Read-only `sd:/halo-wii-memory/arrays.log` has matching BEGIN/END; normalized UTF-8
LF with final LF SHA256
`8aa9406895e986e8d482a248f0f65f531bbe3aab15dc67ea3231850eb9efcb83`.
The owned process faults naturally after 21.026s, unsigned 3221226505 /
signed -1073740791 / `0xc0000409`, with no timeout or forced cleanup.

Sanitized [host](2026-10-07-packet-arrays-host.json),
[build](2026-10-07-packet-arrays-build.json) and
[runtime](2026-10-07-packet-arrays-runtime.json) records retain source/compiler/SDK/
artifact identities, all report sections, policies and strict/probe scopes.
Guest completion is separate from the known host teardown failure.

The bounded array/capacity policy is qualified only in this isolated diagnostic.
Production migration, actual packet structs/callers, group trailer/class/error/
capacity contracts, peer interoperability, original unaligned accesses and broader
ABI remain open. Next bounded work should exercise actual packet-group boundaries
with preserved consumed-size/trailing semantics before a production proposal.
HWI-005/006 remain blocked; physical Wii, full game memory and gameplay are untested.
No Dolphin fork, profile/security/console or production changes were made.
