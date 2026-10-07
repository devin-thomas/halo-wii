# Packet version contract audit and next diagnostic correction

The [measured comparison](2026-10-07-packet-dispatch.md) leaves the same 16
version failures on host and PPC. Source/caller review confirms the mismatch,
but establishes no deployed contract for version-gated fields. This audit
proposes a separate opt-in diagnostic correction; no correction is implemented
or qualified by this record. Source reviewed is unchanged from
`b57a1085c00432c97aa3050d2321699b3977890c`.

## Existing boundary

| Excluded field | Encoder wire placeholder | Decoder behavior |
|---|---|---|
| Bytes, shorts, longs, int64s, raw | Exactly `field.count` zero bytes, independent of element width | Zero native `field.size`; consume no placeholder |
| String | One NUL byte | Zero native reserve; consume no placeholder |
| Data | Zero count encoded with the integer width selected by `field.count` | Zero native reserve; consume no placeholder |
| Array | Zero count prefix, but child schema is not skipped | Zero header size, then erroneously visits child schema |
| Native pad | No wire bytes | Zero native reserve |

See [excluded encode](../../../source/memory/data_packets.c#L411) and
[excluded decode](../../../source/memory/data_packets.c#L572). Public
[decode](../../../source/memory/data_packets.c#L185) permits older versions,
reports success when overflow is clear, and returns the wire cursor as its
optional decoded-size output. It permits trailing bytes. The packet-group
[caller](../../../source/memory/data_packet_groups.c#L269) discards that output,
so it does not detect the demonstrated under-consumption through this API.

The array verifier [advances to the child END](../../../source/memory/data_packets.c#L280)
and stores the full reserve there. Neither excluded arm performs that schema
advance. A decoder-only array change cannot repair the original encoder's
child traversal. Arrays need a separate paired correction and nested tests.

Reviewed actual [network message fields](../../../source/networking/network_messages.c#L192),
[key-agreement fields](../../../source/bungie_net/common/key_agreement.c#L88)
and [player-action definitions](../../../source/networking/network_game_globals.c#L214)
use version 1 with zero minimum/maximum gates. Message creation requests
[version 1](../../../source/networking/network_messages.c#L832). No assignments
introducing field gates were found outside diagnostic fixtures. Packet version
is distinct from the native network version; the upstream
[network-version refusal policy](../../../port/linux/NETCODE.md#L135) supplies
no authority for these historical placeholders.

Local history retains the encode/decode mismatch from reconstruction commit
`eca5cb1a1619588a4e1e28a93940c3f111d47ef8` (2026-07-15). Matching commit
`800d746dd663d003b460832c44f27fb36fbf2d62` (2026-08-13) restores the verifier's
indeterminate-size behavior and separates excluded-pad handling. Later length
hardening did not resolve this version mismatch. This explains provenance;
it does not establish an intended compatibility contract.

## Bounded next correction

Preserve original packet bodies, encoder and golden `01 00 00 e7`. Add a third,
explicitly isolated decoder whose excluded-field arm reads the encoder's exact
placeholder span using checked **raw-byte** scalar services. Scalar/raw fields
consume `field.count`, string consumes one byte, and data consumes the existing
integer-selector width without interpreting a payload count or mutating input.
Pad consumes nothing. After a successful read, zero the verified native extent.
For a failed read, retain sticky overflow, prior completed writes and the
current native field unchanged. That failure behavior is an explicit new
diagnostic policy, not an established engine contract.

The first candidate should consume the fixed placeholder span without adding
zero-content rejection. Test nonzero placeholder contents and record this
choice explicitly; requiring zeros would be separate validation policy.
Do not add global exact-consumption enforcement: trailing-byte acceptance is
separately measured behavior. Keep excluded arrays unqualified rather than
silently traversing their child fields. All test fields must remain eligible
at the definition's own version to avoid the
[uninitialized verifier path](../../../source/memory/data_packets.c#L220).

Required next evidence: each excluded scalar/raw/string/data/pad type followed
by an eligible tail, truncation at every placeholder boundary, sticky overflow,
partial native output, input preservation and full-buffer canaries at offsets
0-7. The existing failed assertions must remain in the reference/candidate
comparison; the new decoder should separately demonstrate consumption 4 and
tail `e7`. Only after host/PPC execution, independent review and a deliberate
verifier/array/compatibility policy should production integration be proposed.
