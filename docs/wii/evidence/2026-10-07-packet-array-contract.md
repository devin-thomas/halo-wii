# Packet array and group source contract

This is a read-only source audit against
`8da0070c728093e3aa5f6f52b36eaa76584168af`. The actual engine sources remain
unchanged. The layouts below are derived from source, not new execution
results. The planned array diagnostic is isolated from production; this note
does not qualify nested arrays, gated arrays, network peers or Wii hardware.

## Real schemas and callers

Source/port searches found two generic packet array schemas, both in
[network_messages.c](../../../source/networking/network_messages.c#L449).
Neither contains nested arrays. All their fields use minimum/maximum gates
zero; their definitions use packet version 1 through the
[schema macros](../../../source/networking/network_messages.c#L192).
No deployed gated or nested array contract was established by this audit.

| Schema | Array capacity | Native layout with 32-bit long | Child layout |
| --- | --- | --- | --- |
| [Server game update](../../../source/networking/network_messages.c#L450) | 128 | Three longs, pad2, short count at offset 14, children at offset 16; array reserve 4098; total 4112 (`0x1010`) | Longs6, shorts3, pad2: native stride 32, wire stride 30 |
| [Client game update](../../../source/networking/network_messages.c#L486) | 4 | Long, pad2, short count at offset 6, children at offset 8; array reserve 130; total 136 (`0x88`) | Same child layout |

The count prefix is one wire byte for both real capacities. The scalar service
selects a byte for maximum count <=255 and a short for larger maxima; its
short decoder returns a signed value. See
[integer encode](../../../source/memory/data_encoding.c#L158) and
[integer decode](../../../source/memory/data_encoding.c#L479). Schema counts,
native array counts and cached extents themselves are signed shorts.

[Native limits](../../../port/linux/include/halo_port_limits.h#L21) set 128
network players; local player capacity remains four. Shared datum identities
also require [matching peer capacities](../../../port/linux/include/halo_port_capacity.h#L8).
This note does not authorize changing advertised capacities or protocol identity.

Empty runtime counts are meaningful. The current server source explicitly
[sets player_count to zero](../../../source/networking/network_server_manager.c#L2384)
before creating its game-update message. The receiving client
[uses update number and time](../../../source/networking/network_client_manager.c#L1600),
not that message's actions. This documents source behavior, not a new observed
session. Client updates use
[local_player_count](../../../source/networking/network_game_globals.c#L622);
the server independently
[checks their count against its per-machine maximum](../../../source/networking/network_server_manager.c#L2264).

## Legacy array metadata and traversal

The verifier recursively computes a child stride, computes
`sizeof(short) + maximum_count * child_stride`, then advances its field pointer
to the child END before storing the full array reserve. Consequently the
array header keeps its prior size, normally zero in fresh real schemas, and
the child END holds the reserve. See
[verification](../../../source/memory/data_packets.c#L275) and the
[size assignment](../../../source/memory/data_packets.c#L294).

Included encode/decode rely on this placement: they skip the complete child
schema, including its END, and then advance native storage using the END's
reserve. See [encode traversal](../../../source/memory/data_packets.c#L365)
and [decode traversal](../../../source/memory/data_packets.c#L533).
The skip still happens when runtime count is zero. Unused native elements are
not initialized by this path. An empty child schema has stride zero; a positive
schema maximum still reserves its short count prefix. Moving the reserve onto
the array header alone would break these consumers.

Excluded runtime arrays have different, defective behavior. The encoder
[emits a zero count prefix](../../../source/memory/data_packets.c#L428) but
does not skip children. The decoder
[zeroes only the current cached size](../../../source/memory/data_packets.c#L571),
consumes no prefix and likewise does not skip children. Both then walk children
as parent fields and stop at the child END, potentially missing subsequent
parent fields. Excluded children inside an included array also retain the
previously documented placeholder-consumption mismatch. These paths are not
evidence of an established interoperable version policy.

Original first-excluded verification reads an indeterminate size, while later
excluded verification reuses the preceding size, as the
[source comment records](../../../source/memory/data_packets.c#L220).
Missing END and unbounded recursion can read beyond accessible schema storage.
Invalid types/counts and size mismatches can terminate through assertions.
Native count reads/writes use typed short pointers, so alignment and the
recorded compiler aliasing policy matter. Safe original comparisons require
bounded accessible schemas, representable extents, aligned prefixes and
nonfatal input cases. The existing harness records
[`-fno-strict-aliasing`](../../../tools/wii/run_memory_subset.py#L132).

## Group boundary remains separate

The [network group](../../../source/networking/network_messages.c#L588) has
35 packet types, eight classes and 4352-byte encoded/decoded maxima. The
[key-agreement group](../../../source/bungie_net/common/key_agreement.c#L190)
has two types, one class, maximum decoded size 96 and encoded size 128.
All their actual entries have definitions. Network message creation
[requests packet version 1](../../../source/networking/network_messages.c#L832).

Groups append a one-byte plain-char packet type as a trailer. Their
[initialization check](../../../source/memory/data_packet_groups.c#L116)
allows native size plus trailer to equal the encoded maximum, whereas
[trailer append](../../../source/memory/data_packet_groups.c#L162)
requires encoded size plus trailer to be strictly less than that maximum.
The codec narrows the long group maximum to short for packet encoding.
These conditions must be preserved or explicitly changed in a separate
group diagnostic; native size is not an exact encoded-size estimate.

[Group decode](../../../source/memory/data_packet_groups.c#L254) checks the
trailer type/class, then removes one byte from the caller's supplied length
before payload decode. That length change also survives payload failure.
It discards the payload decoder's consumed-size output. Null-definition
entries can decode successfully without updating payload/version, although
none occur in the audited real groups. The group error string is shared and
[retrieved once](../../../source/memory/data_packet_groups.c#L129).

The legacy APIs have no schema bound or actual native destination capacity.
Their callers must supply a truthful accessible wire extent and sufficient
native storage. Actual handlers allocate the corresponding structs, for
example the [server-update receiver](../../../source/networking/network_client_message_handler.c#L1291).
Group maxima alone cannot establish that an arbitrary supplied buffer is large
enough. Packet trailing-byte acceptance and exact wire consumption remain
separate questions; this audit proposes no global exact-consumption rule.

## Planned isolated candidate policy

The next diagnostic should take explicit accessible schema bounds, native
capacities and wire read extents, validate before traversal, and use checked
arithmetic for every child stride, reserved array extent and total. Its schema
and recursion limits must be declared and tested. Real schemas have depth one;
synthetic deeper cases do not establish deployed nesting support.

The planned candidate uses stable native reserves for all versions, with paired
encode/decode traversal over a validated snapshot. That is deliberately
different from the earlier standalone flat verifier's own-version rule that
assigns excluded fields size zero. It also differs from legacy child-END size
placement. Mixing either metadata representation with the other codec's
traversal would be unsafe. Excluded arrays require explicit paired handling
of their prefix, child-schema skip, reserve and following fields; version gates
must not silently reactivate fields outside the selected native layout.

Original bodies, fixture snapshots, golden bytes and failed version assertions
remain independent references. No production integration or compatibility
claim follows from this source audit.
