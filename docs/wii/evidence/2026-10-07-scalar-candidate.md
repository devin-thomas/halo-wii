# Explicit scalar wire-order candidate

An opt-in diagnostic adapter passes 336 checks on both the host and PPC guest:
the unchanged 128-check fixture plus 208 edge checks. Its bytes match the
independent x86 golden record at all offsets 0-7. The same PPC executable also
runs the preserved engine reference, reproducing its 16 failures out of 128.
This qualifies the tested adapter, not a production engine boundary fix.

## Identity and commands

Source: `a4eaa5e82c337ed33d9df8116990c85ac555e77c`.
Build ID: `4eddab630c6ffab9`.
DOL SHA-256: `2f6de8e29e92c0728573711fc46913c3b00251cc92e030af791205907903cf9f`.
See [Wii build](2026-10-07-scalar-candidate-build.json),
[host run](2026-10-07-scalar-candidate-host.json) and
[runtime comparison](2026-10-07-scalar-candidate-runtime.json).

```powershell
python tools/wii/run_memory_subset.py --cc C:/msys64/ucrt64/bin/gcc.exe --candidate --output .local/wii-memory-candidate-host
python tools/wii/run_memory_subset.py --cc C:/dev/resources/toolchains/devkitpro/devkitPPC/bin/powerpc-eabi-gcc.exe --wii-devkitpro C:/dev/resources/toolchains/devkitpro --candidate --output build/wii-memory-candidate
```

The host passes reference and candidate. Five existing LLP64 conversion
warnings remain in the extracted reference; the narrow host exception still
reports them. Official PPC GCC 16.1 compiles/links with warnings as errors and
no diagnostics. ELF/DOL validation and SDK/input/artifact identities are
recorded. Candidate source, wrapper and original extracted-source hashes agree
between host and PPC. The original fixture bytes, function bodies and SWAP8
macro match the [preserved failing baseline](2026-10-07-scalar-wii.md).

## Caller contracts and candidate behavior

Textual caller review covers `data_encoding.c`, `data_packets.c` and the AIFF
importer. These contracts constrain a future integration:

| Boundary | Contract retained in the candidate |
|---|---|
| Size selector | `1` means raw bytes; `-2/-4/-8` mean scalar element widths. |
| Null source | Zero-fill the selected extent. Packet version filtering depends on this. |
| Decode | Mutate wire storage into native representation, return its interior pointer and advance the cursor. Packet decoding copies that representation. |
| Integer maximum | Select width; do not clamp the value. Short decoding remains signed (`ffff` returns `-1` at maximum 65535). |
| Failure | Truncation or sticky overflow leaves the buffer and cursor unchanged; scalar wrappers return zero. |
| Cursor assertion | Encode requires an offset strictly before the end; decode permits equality and zero-count one-past return. |
| Generic reversal | Preserve unconditional reversal: the AIFF sample importer also calls `byte_swap_memory`. |

Relevant source: [scalar encoding](../../../source/memory/data_encoding.c),
[packet field dispatch](../../../source/memory/data_packets.c) and
[AIFF importer](../../../source/sound/sound_import/sound_aiff.c).

The adapter lives only in `port/wii/abi/memory_subset/candidate.c`. Explicit
unsigned byte shifts define big-endian wire order. Multi-byte native values
are loaded/stored through `memcpy` into aligned locals; scalar decode never
dereferences a typed pointer into wire storage. Original new-state helpers
remain linked. The runner recompiles the unchanged fixture through adapter
names, while separately linking the original fixture and extracted functions.
Generic swaps, engine structs and production caller code are unchanged.

The adapter deliberately gives malformed counts/selectors a checked sticky
overflow result, instead of relying on the reference's invalid-input
assertion/undefined paths. Remaining capacity is checked using subtraction
and division before cursor addition, multiplication or pointer arithmetic.
State/maximum assertions stay fatal. This malformed-input policy is an
isolated candidate decision, not an approved production semantic change.

Added checks use independent golden input, full 40-byte snapshots and both
source/destination offsets 0-7. They cover signed 16/32/64-bit patterns,
two-element arrays, native decode mutation/pointer identity, source preservation,
null zero-fill, raw-byte copying, zero counts, signed integer dispatch, negative
counts, invalid selectors, maximum short counts and near-LONG_MAX rejection.
The original fixture retains truncation/sticky-overflow and canary checks.
Independent review found no remaining actionable defects after correcting a
Wii include-formatting error; the recorded clean PPC build includes that fix.

## Measured comparison and independent reproduction

Stock Dolphin 2609, executable SHA-256
`1ed5e3f63f08b672133dade2c570e87ea4fa7fac2f6a65a118cc300a18dc0dac`, ran:

```text
Dolphin.exe --user <isolated-profile> --batch --video_backend D3D --exec memory_subset.dol
```

The existing isolated profile/settings did not change. Read-only SD extraction
of `halo-wii-memory/compare.log` finds matching BEGIN/END build IDs and:

| Result | Host | PPC guest |
|---|---|---|
| Original reference | 128 checks, 0 failures | 128 checks, 16 failures |
| Candidate, unchanged fixture | 128 checks, 0 failures | 128 checks, 0 failures |
| Candidate, added edge checks | 208 checks, 0 failures | 208 checks, 0 failures |
| Candidate wire | `5a1234123456781122334455667788` | `5a1234123456781122334455667788` |

The PPC reference retains `5a3412785634128877665544332211`, with exactly the
two original failure names at every offset. `REFERENCE END result=1` records
that failure. The outer `END MEMORY ... result=0` means **candidate plus edges**;
it does not mean the reference or host shutdown passed.

Guest transcript SHA-256 (UTF-8, LF separators and one trailing LF):
`10e0ec2f6a79f2fc9be079cde950a17121db2c8380181ea0781bad0a890c019f`.
The process naturally exits after 5.841 seconds with unsigned `3221226505`,
signed `-1073740791`, hex `0xc0000409`. No timeout or forced cleanup occurred.
Clean Dolphin shutdown still fails separately.

An independent coordinator launch of the original DOL also reproduced the
128/16 failure with build `5b8f8ab099f896d1` and original DOL SHA-256
`bf84725b88f55e307ba5caa0731cb66f86ec20ed8dd93d82b67f0baba332d3d5`.
Its locally verified transcript matches the original report exactly, including
LF-normalized SHA-256
`ceeb0fb9f209fa3fddb89c8cd4d5aefe8b47846472713bf379789157504a59eb`.
The coordinator reported the same host exit after approximately six seconds;
the runtime record distinguishes this reported launch from builder execution.

## Limits and next boundary

The candidate assumes truthful buffer extents and nonoverlapping encode
input/output. Original reference typed accesses remain unchanged. This proves
the listed diagnostic scalar paths under emulation; physical Wii is untested.
Full packet schemas/structs, structure swapping, HS/save policy, cache layouts,
callbacks, intrinsics/atomics and gameplay math remain outside this adapter.

Next work should exercise actual packet field dispatch against authored byte
goldens, including version padding, variable sizes, signed lengths and native
destination layout, before choosing a production boundary integration. HWI-005
and HWI-006 remain open. No Dolphin fork, patch or profile matrix was performed.

The subsequent [actual packet comparison](2026-10-07-packet-dispatch.md) now
executes this adapter through the original dispatch bodies. It removes the
measured PPC endian failures, but preserves 16 older-version consumption/tail
failures on both host and PPC. Production integration remains unqualified.
