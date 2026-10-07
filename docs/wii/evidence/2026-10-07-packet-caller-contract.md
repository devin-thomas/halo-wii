# Packet caller and outer framing source contract

Audit basis: `827861ee8c42525661c2ff6d4b5df3acf8840cb2`. This is source
inspection only. No new execution, socket, crypto, complete-engine or hardware
acceptance follows. Actual caller bodies and the earlier
[group contract](2026-10-07-packet-group-contract.md) remain unchanged.

| Audited file | SHA256 |
| --- | --- |
| `source/bungie_net/common/message_header.c` | `1d597d5a0b676c4ebe709ba0001ac4d6230492e33076badf2018db0a440458a8` |
| `source/bungie_net/common/message_header.h` | `412470636014fdb465924d0dddb8b908952bfb0a2b2051c9a04d0bd29c5b40f5` |
| `source/networking/network_messages.c` | `4af343112304205ff504c7acc4bc85e090d4f17a5d9ca0dd7633e46630d440ca` |
| `source/bungie_net/common/key_agreement.c` | `eabd9afa0679f7c8a8ecb45e879a714b61c7f064437f9606d7bd1d33efbe1971` |

## Header bits and byte order

[Engine typedefs](../../../source/cseries/cseries.h#L180) make `byte` unsigned
char and `word` unsigned short. The header is a
[word alias](../../../source/bungie_net/common/message_header.h#L41). The
bounded proof must establish 8-bit bytes, 16-bit word/short and 32-bit long;
LP64 is not an equivalent native layout.

| Header bits | Meaning | Source contract |
| --- | --- | --- |
| 15-4 | Total outer frame size, including two-byte header | Maximum `0xFFF` (4095) |
| 3-2 | Outer message type | Builder accepts 1-3; packet type is 3 |
| 1-0 | Flags | Builder accepts 0-3 |

The exact [get/set macros](../../../source/bungie_net/common/message_header.h#L33)
extract these numeric bits. `SET_MESSAGE_FLAGS` clears the low two bits then
ORs the supplied flags without validation or masking; invalid flags can
modify type/length bits. Its actual key caller supplies only `FLAG(1)` (2).
These numeric operations require a host-order header at the call site.

[Header construction](../../../source/bungie_net/common/message_header.c#L50)
first reads `*msg`, preserves its low nibble while replacing length, then
replaces type and flags. All 16 final bits are determined, but the initial
load still requires an initialized, aligned accessible word. Length/type/flag
assertions are sequential: invalid later inputs can leave earlier writes.
This is not an atomic rejection routine. `create_message` supplies flags 0.

[Both byte-order branches](../../../source/bungie_net/common/message_header.c#L70)
perform the same unconditional `(value << 8) | (value >> 8)` assignment to
word. There is no host-endian test. Applying it twice restores the numeric
word, but does not establish portable network-order bytes. For example,
length `0x23`, type 3, flags 0 produce numeric header `0x023c`; swapping
produces `0x3c02`. Its memory bytes are `02 3c` on little-endian and `3c 02`
on big-endian under the stated word representation. This is source-derived,
not measured caller execution or a verified peer negotiation. A diagnostic
must retain the original swap as a reference and label any explicit
big-endian wire adapter separately.

## Original create_message and safe extraction

The exact [creator](../../../source/bungie_net/common/message_header.c#L87)
narrows `unsigned long data_size + sizeof(word)` into a short before either
capacity or header checks. With a supplied buffer it asserts
`buffer_size >= message_size`; otherwise it allocates that narrowed size.
It then builds a header and copies `(word)data_size` bytes if data is nonnull.
It does not swap the header, append a group trailer, validate packet class,
prove accessible extents or zero a null-source payload.

Safe isolated original invocations require:

- Valid outer type 1-3 and `data_size` 0-4093, so total size 2-4095 is
  representable as short/word and accepted by the header assertion.
- A supplied writable buffer with truthful capacity at least the total,
  aligned for word and with an initialized header word before the call.
  Keep the allocation branch out of this bounded reference: an ordinary
  uninitialized allocation does not satisfy the header's first-load condition.
- For nonnull data, at least `data_size` accessible source bytes, sufficient
  destination extent and nonoverlap. Preserve source and full destination
  snapshots; null data deliberately leaves payload bytes unchanged.
- If extracting the actual [csmemcpy](../../../source/cseries/cseries.c#L588),
  both regions must belong to one backing array for its relational
  nonoverlap assertion at line 595. Disjoint unrelated arrays do not make
  those pointer comparisons defined portable C. Retain the actual helper
  body/assertions, or explicitly record any shim difference.
- The backing arrangement must satisfy alignment and the established
  compiler/object-access policy; a byte pointer alone proves neither typed
  word access nor a complete native struct layout.

Oversized/wrapping data sizes, invalid type/flags, null/undersized output and
overlap are diagnostic rejection cases, not safe nonfatal original oracles.
The creator does not take an independent readable source extent. Its source
and destination parameters remain a caller contract even when their numeric
sizes pass assertions.

## Actual long/short size bridges

The declarations and operations below are the actual source idioms. They can
be isolated with controlled scalar outputs without extracting the engine,
socket or crypto bodies. They are representation experiments, not numeric
conversions and not whole-caller execution.

```c
union network_game_message_size { long value; short encoded; };
union key_agreement_packet_value { long value; short encoded; };
/* Each caller initializes value, passes &encoded, then reads value. */
```

| Caller | Initialization | Short-member use | Subsequent long reads |
| --- | --- | --- | --- |
| [Network declaration](../../../source/networking/network_messages.c#L275) | [value = sizeof(encoded_message)](../../../source/networking/network_messages.c#L682), allocation 4352 | [Output pointer](../../../source/networking/network_messages.c#L832) passed through positive-size wrapper | [Outer limit](../../../source/networking/network_messages.c#L837), [create_message](../../../source/networking/network_messages.c#L843) |
| [Key declaration](../../../source/bungie_net/common/key_agreement.c#L105) | [value = sizeof(encoded_packet)](../../../source/bungie_net/common/key_agreement.c#L412), allocation 128 | [Output pointer](../../../source/bungie_net/common/key_agreement.c#L416), no equivalent initial-positive assertion in key wrapper | [create_message data_size](../../../source/bungie_net/common/key_agreement.c#L423) |

For these small initialized long values, the overlapping short starts as the
capacity on little-endian and zero on big-endian 32-bit-long/16-bit-short
representation. The network
[encode wrapper assertion](../../../source/networking/network_messages.c#L667)
requires `*encoded_message_size > 0` before calling the group. Thus the
big-endian network caller has a source-derived pre-encode assertion risk,
separate from the post-encode value interpretation.

After a controlled nonnegative short write `s`, little-endian reinterpretation
with these zero high halves yields `s`. Big-endian reinterpretation yields
`(s << 16) | capacity`, where the low half retains 4352 or 128. For example,
writing short 32 into the network union gives long `0x00201100`; writing
short 26 into the key union gives long `0x001a0080`. These equations concern
object representation with the stated widths, not portable C widening.
They must not be presented as executed observations until measured.

The network then enforces total size <4096 before creator invocation. The key
builder has no equivalent checked wide-size bridge before creator's short
conversion. Under the usual low-half narrowing of these 32-bit values, the
key creator would derive short total 130 and copy word-sized length 128,
retaining the original capacity rather than the encoded short length. The
unsigned-long-to-short conversion itself is implementation-defined outside
short range; this conditional consequence must also be measured, not asserted
as portable C behavior. Explicit independent short output followed by checked numeric
conversion is an isolated candidate policy; this audit does not change either
production caller.

## Native identity and allocation association

[Network creation](../../../source/networking/network_messages.c#L672) uses a
4352-byte local packet array and the union above. Its switch checks the supplied
short native size against the `sizeof` associated with each of 35 types; the
default asserts. It requests packet version 1, then writes the outer frame into
a [shared static 4100-byte array](../../../source/networking/network_messages.c#L599).
That output is reused on later creation; callers must copy/consume it before
reuse. Static zero initialization supplies initial bytes, but a byte array's
declaration alone does not prove word alignment/access portability. The real
copy path also uses distinct local/static arrays, so it is not a portable
defined-C reference for csmemcpy's relational assertion.

The [decode wrapper](../../../source/networking/network_messages.c#L858)
asserts positive size and initial type/version, then delegates. It does not
check incoming type equality; class is forwarded from long to short. The
group selects the actual trailer type and checks class, then replaces outputs.
Two different types with the same class can require different native storage.
An incoming expected type or group maximum alone cannot authorize decoding
into a typed destination for another type.

The real dispatch association is supplied before decode:

| Path | Actual-type selection | Native destination example |
| --- | --- | --- |
| Client | [Unsigned trailer switch](../../../source/networking/network_client_message_handler.c#L509) | [Server game-update struct and its type](../../../source/networking/network_client_message_handler.c#L1291), followed by size subtraction and decode |
| Connected server | [Unsigned trailer switch](../../../source/networking/network_server_message_handler.c#L1174) | [Client join-game request](../../../source/networking/network_server_message_handler.c#L1858) |
| Server datagram | [Unsigned trailer and switch](../../../source/networking/network_server_message_handler.c#L1450) | [Game-search destination](../../../source/networking/network_server_message_handler.c#L1458) |
| Key agreement | [Plain-char trailer switch](../../../source/bungie_net/common/key_agreement.c#L286) | [Initiate/finalize native locals](../../../source/bungie_net/common/key_agreement.c#L269), separate decode branches |

The earlier group audit inventories all 34 network decode sites. They select
matching actual structs and subtract the two-byte outer header before group
decode. This association relies on the trailer remaining stable between
selection and decode. A bounded bridge must carry selected identity and native
capacity explicitly, or reject a mismatch before payload IO/control updates;
a class-only synthetic wrapper is not equivalent to those dispatchers.

The key native [public_key](../../../source/bungie_net/common/public_key_crypt.h#L17)
has two unsigned longs. With 32-bit long, its initiate struct contains three
keys (24 bytes) and finalize one (8 bytes). Their
[actual flat schemas](../../../source/bungie_net/common/key_agreement.c#L150)
are six and two longs, version 1. They can be recreated with inert controlled
values for a framing/identity diagnostic; no random generation, cryptographic
acceptance or endpoint exchange is necessary for that limited proof.

## Socket framing and trailing domains

The [datagram maximum](../../../source/networking/network_connection.h#L18)
is 1200; the [reliable maximum](../../../source/networking/network_connection.c#L227)
is 4096. The 12-bit header itself represents only 4095, and the network creator
rejects total >=4096. Native group capacity 4352 is a different limit.

[Outgoing connection write](../../../source/networking/network_connection.c#L815)
asserts that supplied byte count equals the host-order header length, then
swaps the header in place before sending/queuing. The header is not restored
by this body, including ordinary failure. A reusable frame must not be treated
as host order after this call without an explicit state transition.
The [reliable flush](../../../source/networking/network_connection.c#L1897)
dequeues only bytes actually sent, retaining a remainder for later writes;
blocked/failed peers are handled separately. Header packing proof does not
qualify these queue or socket lifecycle paths.

[Datagram receive](../../../source/networking/network_connection.c#L1724)
reads into a bounded local allocation and obtains the endpoint's actual byte
count. Before adding source-address metadata to its queue it
[drops](../../../source/networking/network_connection.c#L1777) counts <3 and
any count not equal to the decoded outer header length. The helper
[copies the header](../../../source/networking/network_connection.c#L1957)
into a local word, swaps and rejects encrypted flag bit 0. Datagram bytes
outside a declared frame are not silently treated as a second frame.

The [queued datagram reader](../../../source/networking/network_connection.c#L643)
peeks its header, checks length >2, <=1200 and <= supplied buffer capacity,
then requires the complete frame plus source-address metadata. Success writes
the host-order word into output and replaces capacity with actual length;
malformed/partial/oversized queue states reset the unreliable queue.

The [reliable reader](../../../source/networking/network_connection.c#L1266)
peeks a header, checks declared size >=2, <=4096 and <= supplied capacity,
and rejects encrypted flag bit 0. Bad framing/capacity marks the connection
closed and stream broken. Incomplete valid frames wait without dequeue;
a complete frame consumes only its declared size and leaves subsequent stream
bytes for future reads. A two-byte frame can pass this reader but is rejected
by packet handlers requiring a trailer. This is not payload exact consumption.

Actual receive callers supply aligned word arrays and reset capacity each
iteration: [client 4096](../../../source/networking/network_client_manager.c#L2551),
[server public datagram 1200](../../../source/networking/network_server_manager.c#L4179),
and [server connected 4096](../../../source/networking/network_server_manager.c#L4249).
These are truthful allocations under the stated word width. Socket/queue
helpers still rely on pointer accessibility supplied by callers.

The [client outer guard](../../../source/networking/network_client_message_handler.c#L466)
returns FALSE for sizes <3 or header mismatch. The
[connected server guard](../../../source/networking/network_server_message_handler.c#L1152)
also returns FALSE; its manager then drops that machine. The
[server datagram guard](../../../source/networking/network_server_message_handler.c#L1420)
drops bad framing by returning TRUE. Server guards read the header before
their later nonnull assertions when size is otherwise eligible; null objects
are not safe original rejection tests. Flagged frames are logged/ignored;
the client additionally allows only advertisement/pong packet types from
[unreliable input](../../../source/networking/network_client_message_handler.c#L488).

Outer length equality does not imply payload exact consumption. Extra payload
bytes before the final group trailer can satisfy outer framing and remain
unconsumed by payload decode. Bytes appended beyond the declared datagram
length are dropped; stream bytes beyond one declared frame remain future
framing input. Preserve these separate domains and the group length-decrement
semantics from the earlier audit; no global exact-consumption rule is added.

## Key framing and initialization limits

The [key builder](../../../source/bungie_net/common/key_agreement.c#L402)
uses a zero-initialized 128-byte packet array, the size union, outer packet
type 3 and caller-supplied word buffer capacity; it sets flag bit 1 after
creation. Its exchange methods use a shared 512-byte buffer. They obtain host
length before [swapping and writing](../../../source/bungie_net/common/key_agreement.c#L253),
and require one write result to equal the whole requested size. The response
branch uses [the same sequence](../../../source/bungie_net/common/key_agreement.c#L313).
That is distinct from network_connection's queued partial-write handling.

The [classifier](../../../source/bungie_net/common/key_agreement.c#L204)
reads header flags/type and writes its type output from `message_size - 1`
before deciding whether flag bit 1/type 3/trailer 0 or 1 identify a key frame.
It has no accessible input extent, minimum-length guard or header-length
equality test. Its type output can change even when the classification is FALSE.
The private [type reader](../../../source/bungie_net/common/key_agreement.c#L354)
likewise trusts header length and returns plain char from its last byte.
The [completion method](../../../source/bungie_net/common/key_agreement.c#L279)
derives a short length from the header and subtracts unsigned sizeof(word)
without a preceding >=3 check. Safe original extraction requires validated
nonnull aligned host-order input covering the declared frame, with a trailer
and sufficient selected native destination. Missing/negative-length cases
must remain candidate-only rejections; the actual key methods prove no guard.

[Game initialization](../../../source/game/game.c#L295) calls
[network packet initialization](../../../source/networking/network_messages.c#L603).
No source/port call site was found for the defined
[key initializer](../../../source/bungie_net/common/key_agreement.c#L344),
classifier, initiation or completion public methods beyond their declarations
and definitions. This search does not establish external reachability or key
startup ordering. Original packet cached initialization and diagnostic
snapshot ownership remain distinct contracts.

## Bounded proof and next prerequisite

An isolated proof can extract the three actual header bodies plus controlled
same-backing-array copy service, execute both actual union idioms with explicit
width/endian metadata, and pair inert real flat schemas with bounded group
snapshots. It should independently record numeric fields, exact bytes, full
canaries, short/long values, selected identity, native capacity and each length
domain. Keep fatal original assertions out of ordinary pass/fail comparison;
record source-derived preconditions separately rather than substituting safe
behavior into the reference. No actual caller body is thereby integrated.

After caller proof, the next prerequisite is qualifying the native schema
bridge against actual ABI layouts and field offsets, immutable dispatch
identity, initialized snapshot/cache lifetime and the declared peer wire
identity. Socket/queue partial-read/write, frame ownership/reuse and startup
ordering need their own bounded integration evidence before production
migration. Key reachability and cryptographic exchange remain unestablished.
This source note neither changes requirements/architecture nor proposes a
production migration.
