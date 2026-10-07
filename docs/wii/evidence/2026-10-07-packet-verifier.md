# Bounded packet verifier diagnostic

Source `8deb5cbcee6d0a064e7ed0c2005d38ef757d0327` adds a separately named
`packet_verifier_diagnostic` and authored fixture. It passes 73 cases / 383
predicates on host and PPC, with identical verifier reports. The original
extracted verifier, earlier fixtures, golden bytes and failed assertions are
preserved. This qualifies the listed standalone flat-schema metadata policy;
it does not integrate a verifier into the engine or qualify array schemas.

## Policy and measured behavior

The caller supplies a truthful accessible field bound, between 1 and 32767,
and must prevent concurrent schema mutation. Every field before the first END
is validated. The diagnostic rejects missing definition/name/fields, negative
declared size, versions/gates outside 0-255, reversed nonzero gates, invalid
types, nonpositive counts, missing END, overflowing native extents and declared
size mismatch. Maximum gate zero remains unbounded. Every array rejects before
reading children, including excluded arrays and truncated child spans.

Native extent is count for pad/byte/raw, count times actual ABI width for
short/long/int64, count+1 for string, and count+sizeof(short) for data. Positive
short counts fit the unsigned intermediate; individual extents must fit signed
short metadata. A subtraction guard checks the total before addition, bounded
by 32767. Excluded fields must also have representable latent extents. These
restrictions and byte-range version rules are explicit diagnostic choices,
not established deployed compatibility requirements.

At the definition's own version, excluded flat fields get size zero. Current
extent resets before each iteration. The diagnostic ignores cached initialized
and field sizes, revalidates the whole bounded prefix, then commits prefix
sizes and initialized only on complete success. Negative/stale cached field
sizes are recomputed; negative declared definition size rejects. Rejection
preserves the complete definition and field table byte for byte, including
preexisting initialized state. END metadata and unused suffix remain ignored
and untouched, matching the original flat behavior. Structured results identify
error, field index, END consumption, total and current extent.

Original executions use defined scenarios only. Twenty-seven equivalence
assertions compare valid flat metadata, including extent limits and first END.
A later excluded field safely reproduces stale size: raw count3, excluded short
count2, byte count1 yields original sizes 3,3,1 and total7. The diagnostic yields
3,0,1 and total4; declared total7 rejects. An initialized original definition safely
skips a negative count; the diagnostic revalidates and rejects it. The original
first-excluded case reads an indeterminate value and remains unexecuted, with
its strict compiler diagnostic retained.

## Executed checks

```powershell
python tools/wii/run_memory_subset.py --cc C:/msys64/ucrt64/bin/gcc.exe --packet-verifier --output .local/wii-packet-verifier-host
python tools/wii/run_memory_subset.py --cc C:/dev/resources/toolchains/devkitpro/devkitPPC/bin/powerpc-eabi-gcc.exe --wii-devkitpro C:/dev/resources/toolchains/devkitpro --packet-verifier --output build/wii-packet-verifier
```

Clean host execution exits 1 because preserved packet sections fail. PPC
compile/link and ELF/DOL structural validation pass. The asset-free executable
writes `sd:/halo-wii-memory/verifier.log`. Both drivers execute every section
before aggregating; no earlier failure skips a section.

| Section | Host predicates / failures | PPC predicates / failures |
|---|---|---|
| Original scalar | 128 / 0 | 128 / 16 |
| Scalar adapter and edges | 336 / 0 | 336 / 0 |
| Original packet dispatch | 1504 / 16 | 1504 / 432 |
| Packet dispatch with scalar adapter | 1504 / 16 | 1504 / 16 |
| Third placeholder decoder | 1504 / 0 | 1504 / 0 |
| Placeholder policy edges | 2912 / 0 | 2912 / 0 |
| Bounded verifier | 383 / 0, 73 cases | 383 / 0, 73 cases |

Packet, policy and verifier sections have aborted=0. The 383 verifier predicates
comprise 70 schema cases times five assertions, 27 equivalence assertions,
three original-behavior assertions and three maximum-bound assertions. Cases
cover all eight flat types, exact/one-over field and total extents, negative
counts/sizes, version endpoints/gates, excluded fields, fresh/cached definitions,
late-error rollback, missing END, guarded tables, ignored suffixes and unsupported
arrays. An allocated table provides a truthful 32767-entry bound with guards.
A positive count above 32767 cannot be represented by the actual short field;
no wrapping cast guesses it. Captured evidence checks case/predicate completeness.

Both previous scalar sections match earlier host/PPC reports exactly. Every
previous packet and placeholder-policy report line matches the
[published policy comparison](2026-10-07-packet-version-policy.md) exactly.
Original scalar function/macro hashes, generated scalar units and complete
packet generation records are unchanged. The new verifier never supplies
metadata to those earlier encoder/decoder fixtures.

Four additional authored helper/fixture compilations pass host/PPC
`-Wall -Wextra -Werror` without warning exceptions or diagnostics. Original
generated verifier/string units still fail strict compilation on both platforms:
maybe-uninitialized field_size, unsigned >=0 and pointer-sign. The runnable
comparison retains existing per-unit exceptions and host LLP64 overflow
exception, reporting 11 host / 6 PPC warnings. Independent source review of
helper, fixture and integration found no actionable issues; runtime execution
was performed by the builder.

## Runtime identity and remaining gates

Build `f0222efaee6171c4`; DOL SHA256
`aae815251c279353568055f934a22074b4d01446287fe33bc6b40b14d77e45eb`.
Stock Dolphin2609 executable SHA256 remains
`1ed5e3f63f08b672133dade2c570e87ea4fa7fac2f6a65a118cc300a18dc0dac`.
The existing isolated profile and D3D settings are unchanged. No Dolphin process
was present before the owned launch. Guest BEGIN/END match the build, aggregate
result 1 preserves prior failures. Read-only SD extraction, normalized to UTF-8
LF with final LF, SHA256:
`44e539dd1b4792d6e3c715c9fd341aea5020e79001e6ba6fe3806594f34612cc`.

The host faults naturally after 5.541s, unsigned 3221226505 /
signed -1073740791 / `0xc0000409`; no timeout or forced cleanup. Guest completion
does not qualify clean shutdown or physical Wii. Sanitized
[host](2026-10-07-packet-verifier-host.json),
[build](2026-10-07-packet-verifier-build.json) and
[runtime](2026-10-07-packet-verifier-runtime.json) records retain identities,
all section reports, strict results and limits. Raw launch/SD records stay local.

Own-version excluded size zero does not establish layout at every runtime
version: gates can activate fields at another version, and the encoder does not
reject every version above the definition's version. Native metadata checks
do not establish destination capacity or wire capacity. Original typed unaligned
access, packet groups and broader ABI remain open. The next bounded work must
validate recursive array schemas/reserve arithmetic and pair encoder/decoder
child skipping, followed by explicit runtime-version native-capacity policy
before any production boundary proposal. HWI-005/006 remain blocked; no engine
integration, gameplay or hardware pass is claimed.

The [subsequent paired array diagnostic](2026-10-07-packet-arrays.md) qualifies
a separate immutable schema snapshot with stable reserves across runtime versions.
This own-version flat verifier, all its existing bodies and its 383 predicates
remain unchanged; the shared extent helper is extended without production use.
