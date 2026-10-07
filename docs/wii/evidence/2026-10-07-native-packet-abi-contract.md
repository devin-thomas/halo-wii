# Native packet ABI and schema association contract

Audit basis: `2884af81191a838dfd7ec0275705ac74ad597b86`. This is source
inspection only. The actual engine, earlier diagnostics and
[caller audit](2026-10-07-packet-caller-contract.md) remain unchanged. Sizes
and offsets below are expected source contracts under explicit ABI conditions,
not newly measured host/PPC results or deployed peer acceptance.

| Source | SHA256 |
| --- | --- |
| `source/networking/network_messages.c` | `4af343112304205ff504c7acc4bc85e090d4f17a5d9ca0dd7633e46630d440ca` |
| `source/networking/network_messages.h` | `0afe85e18c920b0e4c25f1f2bc636f6defe926013c671ef3e36ee337a9c1fa5c` |
| `source/networking/network_client_message_handler.c` | `a828a28587eb2151e9f1f7fcfc573638c99b3f4a4b5de4e9ea52259e109ab927` |
| `source/networking/network_server_message_handler.c` | `e9a73697d783709de18579a911df0d31556b9d45b97cba16e1158da63eac5cfa` |
| `source/game/players.h` | `642bcccba899a69f8cb0d385c7e7f0b5d4a100af5b5f2e88d6cbf3433fdc2cb0` |
| `source/bungie_net/common/public_key_crypt.h` | `f5c92f7c01516ff71675b0d03514e17324a5aa7d5b88110c0c615457e01a7d2f` |
| `source/bungie_net/common/key_agreement.c` | `eabd9afa0679f7c8a8ecb45e879a714b61c7f064437f9606d7bd1d33efbe1971` |
| `port/linux/include/halo_port_limits.h` | `c009fd15f621955767142b9c4b1026407bfa167e36c111d91d5ba382a1adfcf9` |
| `port/linux/include/halo_port_capacity.h` | `5970e9fca8186435460fc273f21b7c6fcc8b54523231257a95cc0037d1457666` |

## The schema's sizeof is not the caller's declaration

The [network definition macro](../../../source/networking/network_messages.c#L192)
sets definition version 1, gates zero, mutable fields and initialized FALSE.
However its `sizeof(structure)` uses local
[opaque byte-array typedefs](../../../source/networking/network_messages.c#L281),
not the typed message declarations in client/server units. Their fixed lengths
make many schema sizes independent of long/wchar alignment. Verifying those
opaque sizes alone does not verify a real caller's `sizeof` or `offsetof`.

Typed declarations are repeated in several translation units. The same name
does not establish identical fields, signedness, packing or semantic meaning.
An extracted catalog must identify declaring unit and exact body, public enum
value, actual group table slot, selected schema/class and caller object. Compare
all these relationships, not only the schema name or total byte count.

The bounded ABI conditions are 8-bit byte, 16-bit short/word/wchar_t, 32-bit
long/unsigned long and 32-bit real. The actual
[real typedef](../../../source/cseries/cseries.h#L180) is float; nested
[angle/vector unions](../../../source/math/real_math.h#L396) must retain their
actual fields. Linux's documented MSVC-like build uses
[32-bit target and short wchar](../../../port/linux/README.md#L733).
The current scalar diagnostic's
[flags](../../../tools/wii/run_memory_subset.py#L161) do not themselves
establish 16-bit wchar. Adding native declarations with ordinary PPC wchar
or LP64 long would change layouts. Record compiler options and primitive
size/alignment measurements before interpreting catalog results.

Native pointer size may differ between an LLP64 host and 32-bit PPC. Pointer
and `size_t` metadata differences are not payload incompatibility by themselves.
[Packet metadata](../../../source/memory/data_packet_groups.h#L30) contains
pointers and a boolean; never serialize its object bytes or compare host
addresses as wire evidence. Measure its internal ABI separately if needed.

## Concrete identity and typed-layout discrepancies

The [public enum](../../../source/networking/network_messages.h#L20) assigns
IDs 8-10 to begin-game, graceful pregame exit and pregame keep-alive. The
[actual group entries](../../../source/networking/network_messages.c#L560)
at those numeric slots instead select keep-alive, begin-game and graceful
pregame exit. All three classes are 2, so a valid class check does not detect it.

| ID | Public/caller type | Actual selected schema | Expected native extent |
| --- | --- | --- | --- |
| 8 | begin-game | pregame keep-alive | Caller long4; schema short2 |
| 9 | graceful pregame exit | begin-game | Both long4, different named identity |
| 10 | pregame keep-alive | graceful pregame exit | Caller short2; schema long4 |

The two-byte keep-alive declaration is at
[client handler line 316](../../../source/networking/network_client_message_handler.c#L316),
and its [actual decode destination](../../../source/networking/network_client_message_handler.c#L1109)
passes that object with enum ID10. The sender similarly declares
[short keep-alive](../../../source/networking/network_server_manager.c#L622).
Consequently direct original schema execution with that real two-byte native
object can overread on encode or overwrite on decode. Do not invoke such an
unsafe original case to obtain a failure oracle. A bounded catalog must report
the mismatch and reject insufficient capacity before payload IO. Do not
silently reorder the table, relabel enum values or enlarge a fixture destination
and call the real association qualified.

This qualifies the earlier caller audit's name-based dispatch association:
the dispatchers select a destination matching the public name, but the
numeric group lookup must also match. Correct outer framing and immutable
trailer identity alone do not prove that final relationship.

ID17 has another discrepancy. The
[client sender declaration](../../../source/networking/network_client_manager.c#L554)
contains short `request_type` (2 bytes), while the
[server declaration](../../../source/networking/network_server_message_handler.c#L445)
contains long `countdown_time` (4 bytes). Its
[schema](../../../source/networking/network_messages.c#L435) is one short.
The [server decode/use](../../../source/networking/network_server_message_handler.c#L2418)
writes only the first short, then reads/casts the long at line 2432. The source
comment acknowledges the unwritten rest. An uninitialized long read is not a
defined-C reference case; initializing a diagnostic destination can expose
preserved high/low bytes but does not qualify the actual caller. Big-endian
short-at-offset-zero is also not numeric long extension.

## Practical bounded ABI catalog

Start with the following actual declarations and schemas. `L`, `S`, `B`, `R`
mean long, short, bytes and raw; `P` is native pad omitted on wire. Offsets are
decimal. Each row needs independent measured `sizeof`, alignment and each
listed `offsetof`, plus full native snapshots and explicit wire goldens.
Do not synthesize anonymous byte arrays in place of these typed declarations.

| Group ID / type | Actual declaration | Schema | Expected native layout |
| --- | --- | --- | --- |
| Network0 search | [Server handler](../../../source/networking/network_server_message_handler.c#L380) | [S2 B8](../../../source/networking/network_messages.c#L333) | Size12; port0, version2, nonce4 |
| Network1 ping | [Server handler](../../../source/networking/network_server_message_handler.c#L387) | [L1 S1 P2](../../../source/networking/network_messages.c#L339) | Size8; timestamp0, port4, padding6 |
| Network3 pong | [Client handler](../../../source/networking/network_client_message_handler.c#L294) | [L1](../../../source/networking/network_messages.c#L351) | Size4; timestamp0 |
| Network4 accepted | [Client handler](../../../source/networking/network_client_message_handler.c#L299) | [L1 S1 P2](../../../source/networking/network_messages.c#L356) | Size8; seed0, machine4, padding6 |
| Network5 rejected | [Client handler](../../../source/networking/network_client_message_handler.c#L306) | [S1](../../../source/networking/network_messages.c#L363) | Size2; reason0 |
| Network6 settings fragment | [Server handler](../../../source/networking/network_server_message_handler.c#L489) and [client](../../../source/networking/network_client_message_handler.c#L263) | [S3 P2 R3584](../../../source/networking/network_messages.c#L368) | Size3592; total0, offset2, length4, pad6, data8 |
| Network12 join | [Server handler](../../../source/networking/network_server_message_handler.c#L430) and [client](../../../source/networking/network_client_manager.c#L601) | [S32 B16 B32](../../../source/networking/network_messages.c#L402) | Size112; wchar name0, token64, hardware80 |
| Network15 settings request | [Server handler](../../../source/networking/network_server_message_handler.c#L438) and [client](../../../source/networking/network_client_manager.c#L621) | [S32 B1 P3](../../../source/networking/network_messages.c#L421) | Size68; wchar name0, machine64, padding65 |
| Network20 server update | [Client handler](../../../source/networking/network_client_message_handler.c#L336), [client manager](../../../source/networking/network_client_manager.c#L569), [server](../../../source/networking/network_server_manager.c#L640) | [L3 P2 array128, child L6 S3 P2](../../../source/networking/network_messages.c#L450) | Size4112; number0, seed4, time8, pad12, count14, children16, stride32 |
| Network25 client update | [Server handler](../../../source/networking/network_server_message_handler.c#L394), [server manager](../../../source/networking/network_server_manager.c#L632), [producer](../../../source/networking/network_game_globals.c#L172) | [L1 P2 array4, child L6 S3 P2](../../../source/networking/network_messages.c#L486) | Size136; number0, pad4, count6, children8, stride32 |
| Key0 initiate | [Actual struct](../../../source/bungie_net/common/key_agreement.c#L93) | [L2 L2 L2](../../../source/bungie_net/common/key_agreement.c#L150) | Size24; prime0, generator8, key16 |
| Key1 finalize | [Actual struct](../../../source/bungie_net/common/key_agreement.c#L100) | [L2](../../../source/bungie_net/common/key_agreement.c#L168) | Size8; key0 |

These twelve rows exercise widths, signed/unsigned declarations, explicit pad,
UTF-16-sized storage, raw payload, real arrays and actual key layouts without
extracting gameplay or crypto execution. Network8/9/10 and17 are separate
catalog-discrepancy cases. This is a bounded subset, not a complete audit of all
35 network types or distributed-netcode records.

The real [player_action](../../../source/game/players.h#L52) child is size32:
control flags0, facing yaw4/pitch8, throttle i12/j16, primary trigger20,
weapon24, grenade26, zoom28 and pad30. The six-long schema spans one integer
and five float object representations; it is not six semantic integers.
Retain actual float/vector declarations and report bit patterns with controlled
values, including signed zero where appropriate. The source size/offset asserts
at lines 64-69 are compile requirements, not evidence they have run on Wii.
Original typed reads also depend on alignment and the established aliasing
policy; round-trip equality cannot prove unaligned access safety.

The [network_player](../../../source/game/players.h#L71) contract is size32:
12 short-wchar units at0, primary color24, icon26 and four signed-char
identities28-31. Several player schemas use S12 S2 B4. Unsigned byte transport
does not remove the native signed-char/NONE semantics or capacity128 bounds.
The server's byte-array updates and client's typed actions must agree on these
strides and on count signedness; unused array reserves remain untouched by
included decode, as recorded in the earlier array audit.

## Raw records are a separate endian boundary

Network2 advertisement is [raw276](../../../source/networking/network_messages.c#L346),
although its [actual typed struct](../../../source/networking/network_server_message_handler.c#L408)
contains native words, wchar units and a long-bearing map. Under the stated
ABI its expected offsets are port52, version54, platform56, game_name58,
reserved90, map116, engine248, counts250/252/254, variant256, flags258,
token260; size276. The XDK
[transport declarations](../../../port/include/xdk/xdk_pdb.h#L794) supply
XNADDR12, XNKEY16 and XNKID8 under that ABI. This advertisement object is
different from `network_advertised_game`, whose unrelated UI/cache offsets
must not be substituted.

Raw copying does not normalize these nested values for a big-endian native
caller. Likewise settings fragments carry chunks of the complete
[network_game](../../../source/networking/network_game_manager.h#L53), not a
fieldwise portable schema. Their 13120-byte native settings record contains
typed fields, PC options and limit-dependent arrays; the
[limit formulas](../../../port/linux/include/halo_port_limits.h#L36) define
offsets and the fragment schema copies each raw chunk unchanged. A scalar
endian-safe adapter or correct fragment header cannot qualify that record's
cross-endian interpretation. Keep raw byte transport and native struct
interoperation as separately labeled evidence.

## Snapshot and initialization lifetime

Actual network fields/definitions are one static mutable aggregate. Public
[verification](../../../source/memory/data_packets.c#L101) skips field traversal
when initialized is TRUE, while group initialization has no independent
validated generation. Editing counts/gates/definition sizes after verification
does not automatically invalidate cached metadata. Legacy array reserves live
on child END and cannot be mixed with a new stable-reserve traversal.

The isolated [array compiler](../../../port/wii/abi/memory_subset/packet_array_policy.c#L34)
requires an unowned plan and truthful bounded schema, copies consumed fields,
computes latent native extents, validates declared size and commits the owned
snapshot only on success. Its
[contract](../../../port/wii/abi/memory_subset/packet_array_policy.h#L30)
does not require or trust the original initialized cache. Destruction frees
nodes and clears the plan. Original later schema edits do not update that
owned snapshot; a new generation needs a new compile and explicit replacement.

The [group contract](../../../port/wii/abi/memory_subset/packet_group_policy.h#L25)
borrows caller-owned entries and compiled plans. All must remain alive and
immutable across selection and payload use. Structural validation is not a
schema provenance hash, lifetime manager or protection against concurrent
mutation. The [caller bridge](../../../port/wii/abi/memory_subset/packet_caller_policy.h#L27)
checks expected trailer identity before typed native IO, but cannot by itself
repair a wrongly associated enum/entry/schema. Catalog provenance must cover
the exact definition, consumed fields, table ordering, capacities and declaring
unit; do not serialize native pointers to represent that association.

## Declared peer identity and provenance limits

There are three distinct version domains:

| Domain | Source declaration and use |
| --- | --- |
| Generic packet schema | Version1 in [network definition macro](../../../source/networking/network_messages.c#L195) and key definitions; public decode returns this output version |
| System-link discovery/message protocol | [Version2](../../../port/linux/include/halo_port_limits.h#L54); search receiver [requires equality](../../../source/networking/network_server_message_handler.c#L1658) and advertisement stores it |
| Native distributed network compatibility | [Version22](../../../port/linux/include/halo_port_limits.h#L62), advertised as explicit little-endian bytes plus flags |

The server [writes native version low/high bytes](../../../source/networking/network_server_message_handler.c#L1714)
into the raw advertisement's reserved area. The client
[reconstructs the word](../../../source/networking/network_client_manager.c#L2472),
and its [compatibility predicate](../../../source/networking/network_client_manager.c#L3061)
requires equal version plus distributed-netcode flag. Zero represents a host
without this advertisement version per source comments. This byte-level word
is explicitly little-endian, unlike scalar big-endian packet services; do not
rename all wire data to one endian convention.

[Capacity authority](../../../port/linux/include/halo_port_capacity.h#L8)
requires equal machine capacities because distributed object/player datum
indices must match. The native limits document that these messages differ
from Xbox messages, including longer arrays and fragmented settings. The
[project provenance](../../../README.md#L12) identifies an Xbox build2342
decompilation plus native ports. The declarations and local source history
establish what this checkout intends; they are not an external normative
protocol specification or captured peer conformance proof.

No deployed peer executable identity, measured peer ABI, packet capture,
cross-endian session or Wii peer acceptance was established in this audit.
Matching version22 numerically cannot prove correct raw layouts, table ordering,
capacity equality or endian conversion. Existing advertised identity and
capacity values remain unchanged; changing or deploying a wire contract needs
separate compatibility authority and evidence.

## Bounded next proof and remaining gate

Measure the twelve-row catalog on host/PPC with actual declarations, dependency
hashes, primitive widths/alignments and all listed offsets. Compare schema
native extents and actual enum/table identities before invoking a codec; retain
8/9/10 and17 discrepancies as explicit failing associations. Use fresh mutable
original schemas only for nonfatal, fully bounded native cases, and independent
immutable snapshots for the candidate. Record field bit patterns, canaries,
unused reserves, source mutation, actual consumed lengths and flags separately.

After that bounded proof, resolve the real raw-record/peer byte contract and
catalog association authority before any production migration. Qualify actual
cache generation, dispatch ownership, transport frame lifetime and socket
partial-IO paths independently. Physical-device and deployed-peer proof remain
open; synthetic or isolated native-layout measurements cannot close them.

## Appendix: plain-char player identities

The actual [network_player declaration](../../../source/game/players.h#L71)
uses plain `char` for machine, controller, team and player-list indices at
offsets 28-31. The source [NONE macro](../../../source/cseries/cseries.h#L89)
is -1, and [player invalidation](../../../source/networking/network_game_manager.c#L312)
assigns it to all four fields. The native source also declares
[CHAR_MIN = -128](../../../source/cseries/cseries.h#L57); that enum constant
does not force compiler plain-char signedness.

The prior [measured group checkpoint](2026-10-07-packet-groups.md#L13)
records signed host plain char and unsigned PPC plain char; its
[runtime report](2026-10-07-packet-groups-runtime.json#L1134) records
`platform_char_min=-128` on host and
[0 on PPC](2026-10-07-packet-groups-runtime.json#L1144). Those are existing
isolated group measurements, not a new execution of network_player callers.
Identical sizeof/offsetof and byte 0xff in a native fixture therefore cannot
qualify the typed sentinel comparisons under default PPC compilation.

On an unsigned-plain-char implementation, assigning -1 stores 255 and normal
integer promotion makes `field == NONE` false. Source-dependent consequences
include skipping the [team default substitution](../../../source/networking/network_client_manager.c#L1905)
and failing the [unused player-slot comparison](../../../source/networking/network_game_manager.c#L381).
The [player-list selection branch](../../../source/networking/network_game_manager.c#L370)
also treats the sentinel as an explicit index. Its following VALID_INDEX check
still guards access; this audit does not claim a resulting out-of-bounds read.

The actual [player validity predicate](../../../source/networking/network_game_manager.c#L430)
checks controller/machine nonnegative and their upper limits. Unsigned char
makes the nonnegative tests vacuous, but the upper checks still reject 255.
The [team range check](../../../source/networking/network_server_manager.c#L2920)
likewise retains its upper bound. These distinctions matter: byte transport
equivalence is not signed semantic equivalence, and not every range check
necessarily becomes permissive.

A future bounded semantic proof must record plain-char policy and test NONE,
0, the maximum valid index and high-half byte promotion/comparisons in these
actual field types. No signed-char compiler override or production declaration
change is introduced in this gate. Existing source hashes remain unchanged.
