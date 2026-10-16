# Compatibility and integration boundaries

## Current evidence

There are **no locally verified Wii game, Dolphin gameplay, physical-Wii, or
mixed-port results**. The [current source audit](UPSTREAM.md) pins OpenCE
`4e8ed2f196e0edd1f2830a4de9841686aabbf466` and network version 22. The initial
planning snapshot used version 17. Record exact tested peer builds instead of
saying 'works with the latest OpenCE'.

## Compatibility matrix to qualify

| Peer / mode | Current status | Required destination |
|---|---|---|
| Wii binary ↔ Wii binary (Dolphin development) | Untested | Initial LAN implementation |
| Physical Wii local co-op / 4P competition | Untested | Wii 1.0 |
| Physical Wii ↔ Wii-binary LAN peers | Untested | Wii 1.0, topology recorded |
| Wii ↔ pinned upstream Windows | Untested | Required post-1.0 |
| Wii ↔ pinned upstream Linux / Android | Untested | Required post-1.0 qualification |
| Other community ports/forks | Not inventoried | Record actual upstream/protocol and test; never assume |
| Wii ↔ original retail Xbox wire protocol | Not implemented | Stretch only |
| Network campaign | Not implemented on Wii | Stretch only |
| Binary Xbox saves | Not implemented | Stretch only |

Original competitive LAN capacity is the floor: 16 players across four machines, up to four locals per machine. Expanded upstream sessions are not a retail-parity requirement. However, support limits must be represented safely in interoperable sessions; a Wii must not join an unsupported configuration and then silently change its rules or state.

## The capacity problem is explicit

The pinned [network limits](https://github.com/OpenCommunityEdition/OpenCE/blob/4e8ed2f196e0edd1f2830a4de9841686aabbf466/port/linux/include/halo_port_limits.h) and [capacity header](https://github.com/OpenCommunityEdition/OpenCE/blob/4e8ed2f196e0edd1f2830a4de9841686aabbf466/port/linux/include/halo_port_capacity.h) define expanded player, actor and object limits. The latter explicitly requires matching capacities across peers because shared datum identities depend on them. Simply reducing constants to fit memory while retaining the protocol version is not compatibility.

The implementation must prove one compatible strategy: fit necessary upstream state, safely represent/sparsely allocate it without semantic changes, or coordinate an upstream-compatible capability/representation change. A private incompatible fork required for all desktop opponents does not meet the stated cross-port objective. Unsupported capacities require clear refusal and a tracked solution, not false advertising.

## Addressing and endianness

Xbox-address-bearing data, save layouts, native pointers and wire datums are different things. Wii has split memory arenas and big-endian CPU behavior; desktop/Android fixed-address assumptions cannot be adopted unexamined. Measure actual usable arenas, prove decoded/relocated pointer graphs and explicitly serialize wire fields. Do not memcpy native C structs into network packets and assume another architecture agrees.

## Gameplay and transport

Reuse the shared upstream session/authority/replication model. LAN discovery and later Internet transport must not become separate games. Record original Xbox behavior versus upstream enhancements. Native local co-op remains required even though network campaign is stretch work. Optional upstream enemy multipliers or larger sessions cannot silently redefine the retail-fidelity baseline.

Wii-native save serialization preserves logical progression/checkpoints and records schema/capacity identity; binary Xbox interchange is not required. See [data policy](DATA-AND-LICENSES.md).

Saved games are kept in generations (HWI-080, `source/saved games/game_state_generations.c`). The checkpoint and the profile's save-and-quit game each have a primary (`savegame.bin`), a candidate (`savegame.new`) and a previous (`savegame.bak`). A save writes the candidate and checks the whole image (chunk CRCs, digest, layout) before it touches anything else. Only then does the primary become the previous (only when the primary itself checks whole and is compatible with this game) and the candidate become the primary. A load tries primary, candidate and previous in that order, and takes the first one that its first pass does not refuse. If the second pass fails after taking part of the state, the game takes the explicit fatal halt path, including in release builds, and no other generation is tried. A save that fails or is refused is no longer deleted. Core exports (`d:\core\<name>`) remain single named files.

The guarantee is about process interruption: if the program stops at any step of a save, a load still finds a checked image of the last save or the new one (`tools/wii/test_save_generations.py` checks every step). It is not a power-loss guarantee. The file API has no flush, and renames and deletes on the Wii's SD/USB FAT driver are not known to reach the medium in order. That needs separate qualification on hardware. In a session, a failed checkpoint write retains the previous checkpoint for revert. An incompatible primary never replaces a compatible backup, and a persistent-load fallback checks the difficulty of the actual selected file before taking state.
