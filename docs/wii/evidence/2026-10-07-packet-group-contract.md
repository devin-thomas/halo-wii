# Packet group boundary source contract

Audit basis: `4446bd41cccfebd127d8f63e79f1a2ac46f77590`. This is source
inspection only, with no new build, execution, network or hardware claim.
The earlier array contract and actual engine sources remain unchanged.

| Audited file | SHA256 |
| --- | --- |
| `source/memory/data_packet_groups.c` | `fc20a56e96059740714796143d6471c50297ba6c44634552ae9c35d2185436d7` |
| `source/memory/data_packet_groups.h` | `53a78a3f8f94b32e8530cd64a05dfa9d8f8dee33bb9c687854243c7a14a8f962` |

## Representation and real groups

The [group structure](../../../source/memory/data_packet_groups.h#L56) uses
signed-short type/class counts, long encoded/decoded maxima and an unbounded
entry pointer. Entries have signed-short classes and optional packet
definitions. The API has no accessible entry/schema bound or actual buffer
capacity parameter. Packet versions and encoded lengths are separate from
the native network protocol identity.

The [group trailer](../../../source/memory/data_packet_groups.c#L73) is a
single plain `char` packet type, with a one-byte byte-swap definition. The
trailer is at the end of the supplied extent, not a leading header. Plain-char
signedness is implementation-defined: with signed char, bytes 0x80-0xff
promote to negative values; with unsigned char, they promote to 128-255.
Therefore upper-half type behavior can differ between host and PPC compiler
policies. This audit does not measure their defaults. The
[diagnostic compiler flags](../../../tools/wii/run_memory_subset.py#L140)
do not explicitly select signed or unsigned plain char.

| Real group | Type/class counts | Native/encoded maxima | Definitions |
| --- | --- | --- | --- |
| [Network game messages](../../../source/networking/network_messages.c#L552) | 35 / 8 | 4352 / 4352 (`0x1100`) | All 35 entries have definitions; classes 0-7 |
| [Key agreement](../../../source/bungie_net/common/key_agreement.c#L184) | 2 / 1 | 96 / 128 | Both entries have definitions; class 0 |

These actual type domains are below 128, so the upper-half signedness
difference does not establish a current real-group failure. A portable
original-reference comparison can restrict valid types to 0-127; support for
larger type domains needs an explicit diagnostic policy and signedness
evidence. This is a packet-type limit, not a player-capacity limit.

## Initialization and capacity

[Initialization](../../../source/memory/data_packet_groups.c#L98) iterates
entries in order. For each nonnull definition it first asserts valid class,
declared native size <= decoded maximum and declared native size plus trailer
<= encoded maximum, then calls the packet verifier. There is no group pointer,
entry-count/bound or positive-maximum validation before the loop. Zero or
negative type counts skip the loop. Null-definition entries skip all three
checks, including class validation.

Initialization is not transactional: earlier packet metadata may already be
committed when a later entry fails. There is no initialized bit on the group.
The [packet verifier](../../../source/memory/data_packets.c#L96) checks its
public definition preconditions, but cached `initialized == TRUE` skips schema
verification. Reinitializing a group does not invalidate cached packet sizes.
Neither initialization nor its native-size arithmetic proves a wire-size
upper bound for every packet/version.

[Group encode](../../../source/memory/data_packet_groups.c#L189) asserts the
group, requested type and output pointers, indexes that type, asserts a nonnull
definition, and passes `(short)maximum_encoded_packet_size` to packet encode.
The long-to-short conversion is unchecked. Its incoming encoded-size value
is not an available-capacity limit: the packet encoder replaces it with bytes
written and trusts the group maximum. The actual output allocation must be at
least that large. Overflowing, negative or otherwise unrepresentable maxima
must not be tested as safe original capacity errors.

[Trailer append](../../../source/memory/data_packet_groups.c#L146) constructs
the trailer pointer before checking either output pointer or nonnegative
length. Its test is `encoded_size + sizeof(trailer) < maximum_encoded_size`,
strictly less, unlike initialization's equality allowance. Append failure
retains the already encoded payload and its length; it does not roll back.
Negative lengths and null size pointers can cause invalid pointer arithmetic
or dereference before an assertion and are not safe original reference tests.

## Decode order, output mutation and trailing bytes

[Group decode](../../../source/memory/data_packet_groups.c#L240) asserts
native/wire/output pointers and expected-class range. It does not explicitly
assert the group pointer, group entry bounds, buffer capacities or a
nonnegative input length. Its length comparison at
[line 254](../../../source/memory/data_packet_groups.c#L254) mixes a signed
short with unsigned `sizeof`: a negative length can convert to a large
unsigned value, pass the comparison and reach out-of-bounds trailer pointer
arithmetic. Negative-length handling is therefore a diagnostic-only rejection
case, not a safe original no-header oracle.

For a nonnegative truthful extent, decode selects the last byte, applies the
one-byte trailer service, checks type range, indexes the entry, then compares
its class. It does not require the input value of `*packet_type` to match that
entry. After type/class acceptance it decrements the caller's length and only
then invokes payload decode, passing no consumed-size destination.

| Outcome | Caller length | Packet type/version and native output |
| --- | --- | --- |
| Zero length, invalid type or mismatched class | Unchanged | Type/version/native output unchanged |
| Valid type/class, null definition | Decreased by one; success | Type set; version/native output unchanged |
| Valid type/class, payload decode fails | Decreased by one; failure | Type unchanged; payload version can be updated and native output can be partially written |
| Valid payload decode | Decreased by one; success | Type and payload version set; decoded native fields written |

The null-definition success branch is explicit at
[line 268](../../../source/memory/data_packet_groups.c#L268). Such entries
cannot be encoded through the public group encoder, which asserts a definition.
They are absent from both audited real groups.

The [payload decoder](../../../source/memory/data_packets.c#L185) accepts
supported older versions, succeeds when overflow is clear and does not require
full consumption. It writes the returned version even on ordinary decode
failure. Thus the group's shortened length is the supplied payload extent,
not the number consumed or an unread-byte count. Extra payload bytes inserted
before the final group trailer can remain accepted and unconsumed. Bytes
appended after an existing complete group frame instead change which byte is
treated as its trailer. These are distinct tests. No global exact-consumption
rule is established or proposed here.

The nominal wire pointer is const, but the group casts its trailer to mutable
storage and scalar payload decoding can mutate encoded bytes in place.
Reference buffers must be writable and independently snapshotted; failed
decode is not an atomic operation.

## Caller inventory and assumptions

The only direct in-tree group clients found are the network wrappers and
key-agreement wrappers. All 34 network payload decode sites were inspected:
16 client sites and 18 server sites. Each passes a matching native destination,
a local short type/version and an extent with the outer word header removed.
The three server datagram sites share a subtraction before their switch.

| Caller family / expected class | Actual decode sites |
| --- | --- |
| Client advertisement / 1 | [787](../../../source/networking/network_client_message_handler.c#L787), [835](../../../source/networking/network_client_message_handler.c#L835) |
| Client pregame / 2 | [874](../../../source/networking/network_client_message_handler.c#L874), [920](../../../source/networking/network_client_message_handler.c#L920), [1020](../../../source/networking/network_client_message_handler.c#L1020), [1068](../../../source/networking/network_client_message_handler.c#L1068), [1114](../../../source/networking/network_client_message_handler.c#L1114), [1199](../../../source/networking/network_client_message_handler.c#L1199), [1251](../../../source/networking/network_client_message_handler.c#L1251) |
| Client ingame / 4 | [1296](../../../source/networking/network_client_message_handler.c#L1296), [1347](../../../source/networking/network_client_message_handler.c#L1347), [1400](../../../source/networking/network_client_message_handler.c#L1400), [1450](../../../source/networking/network_client_message_handler.c#L1450) |
| Client postgame / 6 | [1156](../../../source/networking/network_client_message_handler.c#L1156), [1493](../../../source/networking/network_client_message_handler.c#L1493), [1541](../../../source/networking/network_client_message_handler.c#L1541) |
| Server client-search / 0 | [1460](../../../source/networking/network_server_message_handler.c#L1460), [1490](../../../source/networking/network_server_message_handler.c#L1490) |
| Server client-pregame / 3 | [1867](../../../source/networking/network_server_message_handler.c#L1867), [2159](../../../source/networking/network_server_message_handler.c#L2159), [2200](../../../source/networking/network_server_message_handler.c#L2200), [2239](../../../source/networking/network_server_message_handler.c#L2239), [2290](../../../source/networking/network_server_message_handler.c#L2290), [2351](../../../source/networking/network_server_message_handler.c#L2351), [2423](../../../source/networking/network_server_message_handler.c#L2423), [2479](../../../source/networking/network_server_message_handler.c#L2479), [2533](../../../source/networking/network_server_message_handler.c#L2533) |
| Server client-ingame / 5 | [1528](../../../source/networking/network_server_message_handler.c#L1528), [2575](../../../source/networking/network_server_message_handler.c#L2575), [2622](../../../source/networking/network_server_message_handler.c#L2622), [2661](../../../source/networking/network_server_message_handler.c#L2661) |
| Server client-postgame / 7 | [2720](../../../source/networking/network_server_message_handler.c#L2720), [2762](../../../source/networking/network_server_message_handler.c#L2762), [2805](../../../source/networking/network_server_message_handler.c#L2805) |

The [client entry check](../../../source/networking/network_client_message_handler.c#L464),
[server connected check](../../../source/networking/network_server_message_handler.c#L1149)
and [server datagram check](../../../source/networking/network_server_message_handler.c#L1418)
reject undersized or header-length-mismatched frames before their trailer
dispatch. They trust that the supplied nonnull message pointer covers its
declared accessible extent. Client/server dispatchers read the trailer as an
unsigned byte and select a native destination for that actual type, supplying
the type-specific storage precondition missing from the group API itself.

The [network decode wrapper](../../../source/networking/network_messages.c#L858)
asserts positive input size and nonnegative/positive initial type/version
values, but forwards its long expected class to a short parameter without a
representability check. Actual classes are 0-7. Initial network version values
come from [message version 2](../../../port/linux/include/halo_port_limits.h#L60),
while packet definitions and [network encode](../../../source/networking/network_messages.c#L832)
use packet version 1. Decode replaces the initial version; it is not an
expected-version equality check.

[Network message creation](../../../source/networking/network_messages.c#L677)
allocates the full 4352-byte packet output, initializes the long/short size
union and checks the requested native size in its type switch. After group
encoding it separately enforces the outer transport's 12-bit length limit,
[including its word header](../../../source/networking/network_messages.c#L834).
Group maximum and transport maximum are different contracts. The current
caller size-union representation also needs qualification. The exact
[network declaration](../../../source/networking/network_messages.c#L275) is
`union network_game_message_size { long value; short encoded; };`. The caller
[declares the union](../../../source/networking/network_messages.c#L679),
[initializes its long member](../../../source/networking/network_messages.c#L682)
to the allocation size, passes its short member to
[group encode](../../../source/networking/network_messages.c#L832), then reads
the long member for both the
[outer limit check](../../../source/networking/network_messages.c#L837) and
[outer message creation](../../../source/networking/network_messages.c#L843).
Writing the short member then reading the long member is not portable numeric
widening. Under the expected big-endian PPC representation with 32-bit long
and 16-bit short, the write updates the high half and retains the initial
capacity in the low half. This is a source-derived representation risk, not a
measured wrapper execution result. An isolated group proof does not qualify
this outer wrapper.

[Game initialization](../../../source/game/game.c#L295) calls network group
initialization. The key initializer is
[defined](../../../source/bungie_net/common/key_agreement.c#L344), but no
in-tree source/port caller of it or the public key-exchange methods was found.
Key-agreement runtime reachability and startup ordering remain unestablished
by this inspection.

Key agreement has two payload decode calls:
[initiate](../../../source/bungie_net/common/key_agreement.c#L292) and
[finalize](../../../source/bungie_net/common/key_agreement.c#L324), both class0,
with local native structs and initial packet version1. Their
[parent method](../../../source/bungie_net/common/key_agreement.c#L279)
derives length from the outer header, subtracts its word and reads a plain-char
trailer before dispatch. Unlike the network frame entry points it has no
supplied accessible-buffer extent or preceding minimum-length guard.
The separate [key-message classifier](../../../source/bungie_net/common/key_agreement.c#L204)
also reads `message_size - 1` without checking minimum size or equality with
the header. These methods require validated outer framing supplied elsewhere;
this audit did not establish that integration.

The sole [key group encode builder](../../../source/bungie_net/common/key_agreement.c#L402)
allocates 128 packet bytes. Its exact
[declaration](../../../source/bungie_net/common/key_agreement.c#L105) is
`union key_agreement_packet_value { long value; short encoded; };`. The builder
[declares the union](../../../source/bungie_net/common/key_agreement.c#L409),
[initializes its long member](../../../source/bungie_net/common/key_agreement.c#L412)
to 128, passes its short member to
[group encode](../../../source/bungie_net/common/key_agreement.c#L416), and
[reads its long member](../../../source/bungie_net/common/key_agreement.c#L423)
for outer message creation with the caller's word buffer size. It has the same
source-derived high-half risk. Its public initiate/finalize paths share a
512-byte message buffer. Their transport writes are subsequent operations,
not part of group encoding.

## Shared error lifetime

The [error slot](../../../source/memory/data_packet_groups.c#L88) is shared
across groups and overwritten by each append/encode/decode, including success
which writes NULL. Initialization does not clear it. The
[getter](../../../source/memory/data_packet_groups.c#L129) copies the slot,
clears it and asserts that the copied pointer was nonnull. Read twice, read
after success, or read after an intervening operation is not a safe original
reference sequence. Returned literals have stable storage, but the pending
error association does not. Concurrent access lacks synchronization.

No actual source/port caller of this getter was found. Network wrappers log
generic failure; key wrappers propagate FALSE/NULL. An isolated fixture must
retrieve a known failure immediately and once, and distinguish that legacy
global lifetime from any new structured diagnostic result.

## Safe next diagnostic and integration dependency

Safe original cases require valid nonnull group/entry/output objects, truthful
nonnegative short wire lengths, writable wire storage, adequate native/output
capacities, representable positive group maxima, a valid expected class and
nonfatal payload schemas. Trailer append needs an already valid in-buffer
cursor before its pre-assert pointer construction. Preserve the strict append
boundary, partial writes, length decrement timing, null-definition asymmetry
and shared-error sequencing as observable reference behavior. Invalid wire
types/classes and zero-length decode are safe with these other preconditions;
negative lengths, invalid initialization schemas and assertion-triggering
encode cases are diagnostic-only rejection tests.

The next isolated candidate needs explicit entry/schema bounds, accessible
wire extent, native capacity and output capacity checks before pointer
arithmetic or narrowing. After safely reading the trailer within its truthful
accessible object, it must establish native/wire/control overlap constraints
before payload IO or control updates. Forged or wrapping pointers are outside
that truthful-object precondition. Its selected actual trailer type must
determine the native reserve. An explicit unsigned-byte trailer policy, wider type support,
transactional initialization and per-operation error reporting would be new
diagnostic choices, not silently inferred deployed contracts.

Integration also needs an explicit payload adapter: original group bodies use
legacy packet metadata, while the paired array candidate traverses a validated
snapshot with stable reserves. These representations cannot be mixed without
a defined boundary. The group proof must qualify its one-byte trailer service,
payload failure/output semantics and separately reported consumed size. It
must retain trailing-payload acceptance as a separate measured policy question
rather than add a global exact-consumption rule.

The bounded next integration gate is the actual caller size unions first,
followed by dispatch identity and native allocation/capacity bridging. Group
decode checks class, not the caller's incoming expected type; a wrapper that
passes storage for one typed struct must establish that the selected trailer
type matches that destination or provide its full selected native reserve.
The actual dispatchers supply this association before their calls, but the
group API alone does not. Prove numeric size conversion, this identity and
capacity association, outer framing and initialization ordering before any
production migration. Candidate group code remains isolated; actual caller
bodies are unchanged and no group/caller integration is accepted by this
source audit. This gate does not change requirements or architecture.
