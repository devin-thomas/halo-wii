# Caller size, identity and outer framing diagnostic

Clean source `c49b78be98c3ec51afb33b829a764cd4749500a8` adds a bounded caller bridge around
the existing group policy. Host and PPC each pass 680 reported cases / 4886
checks, failures 0 and aborted 0. Every candidate caller report line agrees;
every earlier section exactly matches its respective published
[group report](2026-10-07-packet-groups.md), preserving all earlier failures.
The case counter mixes named vectors executed at eight offsets with individual
loop iterations; it does not count unique schemas.

The [source contract](2026-10-07-packet-caller-contract.md) covers header bits,
both size unions, native dispatch and socket framing. The new generator retains
three actual message_header.c bodies and the actual two union declarations and
initialization expressions. Authored wrappers inject a short output value 4
instead of running whole network/key encoders. This is an object-representation
measurement, not executed engine networking, crypto or peer negotiation.

## Measured original representation

| Observation | Little-endian host | Big-endian PPC |
| --- | --- | --- |
| Numeric header length 6 / type 3 / flags 0 | 0x006c | 0x006c |
| Original native header bytes | 6c00 | 006c |
| Original network swap bytes | 006c | 6c00 |
| Network initialized long / initial short | 4352 / 4352 | 4352 / 0 |
| Network short4 then long / bytes | 4 / 04000000 | 266496 (0x00041100) / 00041100 |
| Key initialized long / initial short | 128 / 128 | 128 / 0 |
| Key short4 then long / bytes | 4 / 04000000 | 262272 (0x00040080) / 00040080 |

Both original byte-order branches swap unconditionally. A double swap restores
the numeric value, but the first swap does not produce the same wire bytes on
both targets. The PPC network initial short is zero; the actual network encoder
asserts positive input size before group encode. That fatal assertion is a
source-derived caller risk and was not invoked in this diagnostic. Key's
oversized union-value-to-create_message narrowing was not executed. Do not
equate these excerpt observations with whole-caller execution.

Actual create_message safe calls use types 1..3, data sizes 0,1,4,4093 and aligned,
initialized supplied storage. A separate allocation-branch case uses an
explicit authored calloc service that initializes storage even when original
clear=FALSE. This supplies the first-word-read precondition; it does not qualify
engine allocation/clearing policy. The diagnostic memcpy service also does not
execute cseries's relational assertions on pointers to unrelated objects.
Oversized/wrapping original lengths and fatal invalid arguments remain unexecuted.

## Explicit bounded bridge

Encode validates the borrowed group, flags 0..3, selected type, nonnegative
actual capacities and disjoint native/workspace/frame ranges. Group workspace
capacity is signed-short bounded. A separate short receives the group size;
after success it is checked against 4093 and numerically widened to add the
two-byte header. The total is 3..4095, with outer type 3. Header bytes are written
explicitly in big-endian order only after total-size and actual frame-capacity
checks. Workspace partial writes and completed group output remain observable
on failure; the frame stays unchanged until all framing checks pass.

Decode checks accessible extent and length 3..4095, including native overlap
with the complete outer frame. It then loads the two-byte header explicitly,
requires header length equal supplied length, outer type 3 and exact expected
flags, and checks the unsigned trailer against both group range and the
expected native-destination identity before payload IO. Two types sharing a
class cannot substitute for one another. The group then checks expected class
and selected actual native capacity. Its length decrement, version updates,
partial effects, supplied/post-trailer/consumed lengths and acceptance of inner
trailing bytes remain unchanged. Outer bytes after the declared complete frame
are rejected; no global exact-payload-consumption rule is introduced.

Frame size is 0 until a representable encode total is established; capacity
failure exposes that computed total. Decode exposes supplied frame size after
safe length/overlap checks, separately from group consumption. Result/version,
entry table, source fields and borrowed live immutable plan controls must be
separate from data and one another. False extents, forged pointers and concurrent
mutation are outside this diagnostic contract.

## Checks and preserved failures

```powershell
python tools/wii/run_memory_subset.py --cc C:/msys64/ucrt64/bin/gcc.exe --packet-callers --output .local/wii-packet-callers-host
python tools/wii/run_memory_subset.py --cc C:/dev/resources/toolchains/devkitpro/devkitPPC/bin/powerpc-eabi-gcc.exe --wii-devkitpro C:/dev/resources/toolchains/devkitpro --packet-callers --output build/wii-packet-callers
```

Caller mode implies every earlier section; all execute independently before
aggregation. Aggregate host exit1 and guest result1 retain original failures.
Candidate goldens 006c01213200 and 006e01213200, offsets 0..7, full 33024-byte
object snapshots, all ordinary workspace/frame encode capacities, valid outer
frames around every ordinary truncated inner payload, outer length/type/flags,
future version, null definition, same-class identity mismatch, flat reserve 2
versus array reserve 4, invalid count, partial group output, each overlap pair,
outer-header-only alias and boundary-touching ranges are checked. Exact total
4095 / group 4093 / native 4091 encodes and decodes; completed group 4094 / native 4092
rejects before any frame write. Source/cache/entry/plan/node snapshots are intact.
Four plans and allocated reference storage have centralized cleanup.

Six authored unit compilations pass strict Werror without diagnostics on host
and PPC. The preserved original header bodies fail strict compilation with two
type-limits diagnostics each; only generated caller_reference.c receives that
runnable exception. Total runnable warnings are 17 host / 13 PPC, including prior
warnings. Initial authored service format and fixture misleading-indentation
failures are preserved in JSON and fixed without widening warning exceptions.
Coordinator independently reproduced the original header and old service
failures on both targets and clean policy compilation. Its separately sampled
fixture hash exactly matches the final fixture and compiles cleanly on both.
Independent final source review has no unresolved findings. Final fixture SHA256
`662983ec28061a3c2953554870fe02ec2e09f34840452bab4097fd9057a39e78`.

## Runtime identity and next prerequisite

Build `088c37007fa4a533`; DOL SHA256
`3c4b3b7db191652aa24af15562a2d9e2b79e9b5fd2ff42ea9413549ddcbf29dc`.
Stock Dolphin 2609 executable SHA256 remains
`1ed5e3f63f08b672133dade2c570e87ea4fa7fac2f6a65a118cc300a18dc0dac`.
The same isolated profile and CLI D3D override were used; CPUThread, cheats,
overclock and RAM override remain disabled. No settings/profile edits occurred.
Read-only SD callers.log has matching BEGIN/END; normalized UTF-8 LF SHA256
`894aa2d936d3744e1cf22716d7176d95b3aa763118868cd5dfd3accecc2ccaae`.

This DOL completed its guest report, then the host exited with fault
`0xc0000409` after 17.774s, without timeout/kill. A separately owned coordinator
launch of the same DOL/settings reproduced the fault after 19.8477578s and
independently extracted an identical guest report/hash. These are scoped
diagnostic observations.
Earlier 0xc0000409 failures and both group-DOL exit 0 runs remain preserved;
general shutdown recovery is unqualified and no broader shutdown matrix ran.
HWI-005/006 remain blocked. Production, real native structs, full engine,
socket/queue ownership, cryptography, peers, physical Wii and gameplay are open.

Sanitized [host](2026-10-07-packet-callers-host.json),
[build](2026-10-07-packet-callers-build.json) and
[runtime](2026-10-07-packet-callers-runtime.json) records retain reports, exact
hashes, commands, warnings, initial failures and independent reproduction scopes.
The next bounded prerequisite is actual ABI struct layouts/field offsets,
immutable dispatch/native association, initialized snapshot/cache lifetime and
declared peer wire identity. Socket/partial IO, shared-frame reuse and startup
ordering remain separate prerequisites before any production migration.
