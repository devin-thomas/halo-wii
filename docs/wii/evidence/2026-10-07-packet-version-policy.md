# Isolated packet placeholder decoder policy

A third diagnostic decoder passes 4,416 checks on host and PPC: the unchanged
1,504-check packet fixture plus 2,912 new version-edge predicates. It consumes
the legacy encoder's excluded-field placeholders before zeroing native storage.
The original reference and scalar-adapter implementations remain linked and
retain their failures. No production engine code or compatibility policy changed.

| Section | Host checks / failures | PPC guest checks / failures |
|---|---|---|
| Preserved reference | 1,504 / 16 | 1,504 / 432 |
| Preserved scalar adapter | 1,504 / 16 | 1,504 / 16 |
| Third decoder, unchanged packet fixture | 1,504 / 0 | 1,504 / 0 |
| Third decoder, version edges | 2,912 / 0 | 2,912 / 0 |

Every section finishes without abort. Both new sections match host/PPC line for
line. All 96 policy packet wire rows and 72 edge rows match independent goldens.
Host aggregate exit 1 and guest outer `END MEMORY ... result=1` still include
the preserved failures; they do not mean the new policy checks failed.

## Identity and reproduction

Source: `5ccea95d4c40ec44928198f6d88f4694335cea9e`.
Build ID: `f71b3a01e5e4fb0e`.
DOL SHA-256: `b6e5c1ce5f03e8294f2b658481fa92a0e250d5101249a155509dc51ea0733f3e`.
See [host](2026-10-07-packet-version-policy-host.json),
[Wii build](2026-10-07-packet-version-policy-build.json) and
[runtime comparison](2026-10-07-packet-version-policy-runtime.json).

```powershell
python tools/wii/run_memory_subset.py --cc C:/msys64/ucrt64/bin/gcc.exe --packet-policy --output .local/wii-packet-policy-host
python tools/wii/run_memory_subset.py --cc C:/dev/resources/toolchains/devkitpro/devkitPPC/bin/powerpc-eabi-gcc.exe --wii-devkitpro C:/dev/resources/toolchains/devkitpro --packet-policy --output build/wii-packet-policy
```

Both input sets are clean at the recorded source. Original engine bodies,
SWAP8, scalar adapter, scalar/packet fixture bytes and the three preserved
packet/string generated units match the [previous comparison](2026-10-07-packet-dispatch.md).
Its two host and guest sections also match every prior report line.

The third generated packet unit records exactly one controlled transformation:
replace the excluded-decode native-zero arm with a checked helper and stop the
field loop if it fails. The other five extracted packet bodies, including the
encoder and verifier, are identical. Original and transformed decode-body
hashes, replacement count, generated-unit hashes and helper inputs are recorded.
The unchanged fixture is compiled through a separate wrapper for `POLICY`.

Host GCC 16.2 retains eleven warnings; PPC GCC 16.1 retains six. The extra
warning is another copy of the unchanged verifier, with the same per-unit
`maybe-uninitialized` exception. Original string and LLP64 exceptions remain
scoped as [previously measured](2026-10-07-packet-dispatch-strict-compile.json).
Authored policy helper and edges compile with warnings as errors. Potential
verifier UB is unresolved; all executed fields are eligible at definition
verification. ELF/DOL validation and exact input/compiler/artifact identities pass.

## Explicit diagnostic policy

The [reviewed contract](2026-10-07-packet-version-contract.md) determines spans:

| Excluded selector | Raw bytes consumed | Native bytes zeroed |
|---|---|---|
| Bytes / shorts / longs / int64s / raw, count 3 | 3 | 3 / 6 / 12 / 24 / 3 |
| String maximum 4 | 1 | 5 |
| Data maximum 255 / 256 | 1 / 2 | 257 / 258 |
| Pad count 3 | 0 | 3 |

Valid packet counts are positive shorts, so data prefix selection is one or two
bytes. The helper uses checked raw-byte scalar reads without interpreting
placeholder contents. Nonzero placeholders are explicitly accepted. It does
not treat them as variable data lengths or scan a non-NUL string placeholder.
After a successful read it zeros the verified native extent. Failed reads retain
the current field, prior completed writes and sticky overflow, and stop this
field loop. Zero-span pad also honors prior overflow before changing native data.
This failure/acceptance policy belongs only to the third diagnostic.

Placeholder reads leave input unchanged. Eligible multi-byte fields retain the
existing candidate's in-place native mutation. The new edge schemas use byte
lead/tail fields, so their entire input remains unchanged. Existing included
string overflow side effects and trailing-byte acceptance are preserved.

The original golden `01 00 00 e7` remains unchanged: the preserved decoders
consume 2 and output tail `00`, while the third consumes 4 and outputs `e7` at
every offset. Original desired-consumption/tail assertions remain failed in
their original sections and pass separately in the third section.

Nine edge cases place an included `a9` lead and `e7` tail around each excluded
selector at definition version 2 / runtime version 1. They check zero and
nonzero placeholders, trailing bytes, every shorter declared input length and
sticky private dispatch directly at the excluded field, including pad.
Independent hex goldens, full 768-byte snapshots and offsets 0-7 verify native
extents, cursor state, completed writes and all input/output canaries. Definition
fields remain eligible at version 2. No excluded array or invalid schema path is
executed. Independent review found no actionable defects.

## Stock guest execution and limits

Stock Dolphin 2609 executable hash
`1ed5e3f63f08b672133dade2c570e87ea4fa7fac2f6a65a118cc300a18dc0dac` ran with
the unchanged isolated profile and D3D backend. No existing process was present.
Read-only SD extraction of `halo-wii-memory/version.log` verifies matching
BEGIN/END and all four complete sections. Guest report SHA-256 (UTF-8, LF
separators and one trailing LF):
`452c5c2c0ef1782f5e6903e9356fe4e8d36f48fb57bdad1711bb8eddcb00b725`.

The owned process naturally exits after 5.405 seconds with unsigned
`3221226505`, signed `-1073740791`, hex `0xc0000409`; no timeout or forced
cleanup. Host teardown remains a separate failure. A coordinator independently
ran the prior packet DOL and reported the same host exit; builder verified its
entire prior 432/16 guest report and exact transcript hash. That reproduction
is distinguished from this new policy run in the runtime record.

Excluded arrays explicitly return overflow and stop in this diagnostic; this
does not correct their encoder/schema traversal or qualify their native layout.
Verifier indeterminate/stale size, native extent overflow, broader schemas,
packet groups and production compatibility remain open. Actual callers establish
no deployed version-gated contract. Truthful destination capacity is assumed;
original reference typed unaligned wire access and physical Wii remain unqualified.
HWI-005/006 stay open. No Dolphin fork, profile/security/console change or
production integration was performed.
