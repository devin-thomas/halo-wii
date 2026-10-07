# Actual packet field dispatch comparison

The diagnostic scalar adapter removes the measured PPC endian differences
through actual packet dispatch, but packet version decoding still fails on
both platforms. This is a failing packet qualification result, not a production
integration. Original engine bodies and independent expected bytes are retained.

| Implementation | Host | PPC guest | Wire rows matching goldens |
|---|---|---|---|
| Original packet/scalar reference | 1,504 checks, 16 failures | 1,504 checks, 432 failures | Host 96/96; PPC 24/96 |
| Original packets with scalar adapter | 1,504 checks, 16 failures | 1,504 checks, 16 failures | Host/PPC 96/96 |

All four sections finish with `aborted=0`. The candidate host and guest
sections match line for line. Its 1,488 passing predicates do not close the
remaining 16 failures. The host command exits 1, and the guest outer
`END MEMORY ... result=1` covers **both packet implementations**.

## Reproduction and identity

Source: `b57a1085c00432c97aa3050d2321699b3977890c`.
Build ID: `ca56187765c4f434`.
DOL SHA-256: `a5b7cb29e3338aab24ea026cafec455a08f4a4c03ba4a2502615a07736511ed9`.
See the [host manifest](2026-10-07-packet-dispatch-host.json),
[Wii build](2026-10-07-packet-dispatch-build.json),
[runtime comparison](2026-10-07-packet-dispatch-runtime.json) and
[strict compile failures](2026-10-07-packet-dispatch-strict-compile.json).

```powershell
python tools/wii/run_memory_subset.py --cc C:/msys64/ucrt64/bin/gcc.exe --packets --output .local/wii-packet-host
python tools/wii/run_memory_subset.py --cc C:/dev/resources/toolchains/devkitpro/devkitPPC/bin/powerpc-eabi-gcc.exe --wii-devkitpro C:/dev/resources/toolchains/devkitpro --packets --output build/wii-packets
```

Both input sets are clean at the recorded source. Eleven actual scalar bodies,
SWAP8, original fixture and scalar adapter inputs remain identical to the
[prior comparison](2026-10-07-scalar-candidate.md). Six actual packet functions
and five actual string helpers have matching host/PPC body and generated-unit
hashes. Candidate preprocessor aliases select scalar services; packet branches,
native layouts and string helper bodies are unchanged. Authored assertion,
memory and bounded diagnostic-format services remain explicit test limits.

Host GCC 16.2 retains ten warnings: five prior LLP64 selector conversions,
two verifier diagnostics and three string-service diagnostics. PPC GCC 16.1
retains the latter five. Initial strict compilation failed on verifier
`maybe-uninitialized`, unsigned `size>=0` and byte/char pointer signedness.
The coordinator independently reproduced both exact failed compile vectors.
Final exceptions are scoped to the generated packet/string units; other
warnings remain errors. **The verifier warning indicates potential undefined
behavior**, not harmless portability noise. All executed schemas have fields
eligible at definition verification, avoiding that path. Invalid/excluded-at-
definition schemas still require a deliberate verifier fix and policy.

The coordinator also independently reran the host command with a separate
output directory: exit 1 and the exact same 1,504/16 reports. Builder verified
every report line, source/input/compiler/function/generated-unit identity and
the failure exit. Its distinct executable hash is retained separately.

## Covered packet behavior

Authored schemas exercise wire offsets 0-7 with aligned native count prefixes.
The current native layout is 40 bytes: short/long/int64 fields, three raw bytes,
a string reserve, variable data, a nested short array, tail and native padding.
Five independent hex records cover data counts 0-4, array counts 0/1/2 and
strings of length 0/2/4. A 260-byte native schema exercises the two-byte signed
count selector at maximum 256, with counts 0/1/256.

Fresh golden decode is independent of encoder output. Full 768-byte snapshots
check native reserves, canaries, source preservation and input mutation.
Truncated encode/decode checks retain completed fields and their cursor state.
Negative, excessive and truncated signed lengths reject; truncated payload
checks distinguish partial array writes from data-block rejection. Trailing
wire bytes are accepted without consumption, and too-new versions reject after
reading the version byte. Private dispatch observation also confirms that a
string can copy and advance while a prior scalar overflow stays set. This
records existing behavior; it does not approve that failure policy.

Independent review caught a fixture pointer-comparison issue in actual
`csstrcpy`. String decode source and destination now occupy disjoint regions
of one flat aligned byte array. The original helper is preserved. Native count
casts remain aligned, with no blanket packing. Array reserve metadata belongs
to the child END field, as the original verifier implements.

## Preserved version failure

The definition version is 2; one byte and one short are excluded at runtime
version 1, followed by an eligible byte `e7`. Both encoders produce independent
golden `010000e7`: version, two one-byte padding writes, tail. Both decoders
report success, zero the three excluded native bytes, consume only two wire
bytes and copy padding `00` into the native tail instead of `e7`.

`cross_version_desired_full_wire_consumption` and
`cross_version_desired_native_tail_and_canaries` fail at all eight offsets in
both implementations on both platforms. These assertions remain failed; the
expected tail/consumption was not changed to hide the defect. Wire agreement
does not establish version correctness. Actual excluded multi-byte fields
encode `field.count` padding bytes, while decode does not consume them.
Excluded arrays and verifier eligibility have further unqualified hazards.

The PPC reference has 416 additional failures: opposite scalar wire order,
incorrect fresh-golden native mutation and two-byte length interpretation.
The runtime JSON retains each case/assertion histogram and every failure line.

## Stock runtime and remaining gates

Stock Dolphin 2609 executable SHA-256
`1ed5e3f63f08b672133dade2c570e87ea4fa7fac2f6a65a118cc300a18dc0dac` ran:

```text
Dolphin.exe --user <isolated-profile> --batch --video_backend D3D --exec memory_subset.dol
```

No existing Dolphin process was present. The existing isolated settings are
unchanged. Read-only SD extraction of `halo-wii-memory/packets.log` verifies
matching BEGIN/END build IDs. Guest report SHA-256 (UTF-8, LF separators and
one trailing LF):
`3699ee98c04425f0751f2fbb2afed4d64e23f1b156cc41fdff3da9e339bb8f3f`.

The owned process naturally exits after 5.871 seconds, unsigned `3221226505`,
signed `-1073740791`, hex `0xc0000409`, without timeout or forced cleanup.
The separate host teardown failure persists. Raw process/profile records are
local only; published transcripts contain authored diagnostic fields.

Original scalar typed unaligned wire access remains a defined-C qualification
limit. Truthful destination capacity is assumed because packet decode has no
output-capacity argument. Packet groups, all engine schemas, structure swaps,
HS/save policy, cache/callback/atomic/math boundaries and physical Wii remain
unqualified. Next bounded work is the version-padding/decode contract and
actual callers, with a separate opt-in diagnostic correction before production
integration. HWI-005/006 remain open. No Dolphin fork, patch, settings matrix,
console change or production engine rewrite was performed.
