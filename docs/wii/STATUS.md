# Wii port status

**Probe compile pass (2026-10-07):** the asset-free native probe builds with
configure.py/Ninja and official devkitPPC/libogc. Clean, incremental and checkout
paths containing spaces passed. See [compile evidence](evidence/2026-10-07-probe-build.md)
and [build setup](BUILDING.md). Dolphin and physical Wii remain separate gates.

| Scope | Compile | Dolphin | Physical Wii |
|---|---|---|---|
| Asset-free Wii diagnostic | Passed: asset-free probe | Blocked: host teardown | Untested |
| Halo combat slice | Untested | Untested | Untested |
| Complete campaign | Untested | Untested | Untested |
| IR / motion | Untested | Untested | Untested |
| 2P campaign co-op | Untested | Untested | Untested |
| 4P competitive split-screen | Untested | Untested | Untested |
| Wii LAN | Untested | Untested | Untested |
| OpenCE mixed-port play | Untested | Untested | Untested |

The original probe's clean Dolphin exit remains blocked; physical Wii is untested. Guest observations and the host-teardown failure are recorded in [Dolphin evidence](evidence/2026-10-07-dolphin-probe.md). Later scoped diagnostic clean exits are recorded separately in the follow-ups on this page. Synthetic C host/PPC results are [separate](evidence/2026-10-07-abi-fixtures.md). Compact pass labels must identify their specified build/scenario and distinguish untested, failed and blocked hardware results.

Physical Wii validation gates 1.0, not source publication or alpha development. Wii 1.0 does not close the later required cross-port milestone. See [test policy](TESTING.md) and machine-readable [status](status.json).

Further [shutdown diagnosis](evidence/2026-10-07-dolphin-shutdown.md) catches
`ucrtbase!terminate` during CRT exit handling; normal GUI application close also
fails. The exact static object remains unidentified. The [engine ABI audit](evidence/2026-10-07-engine-abi-audit.md)
adds a script-cell endian failure model. All 24 checks now match host and PPC
execution in a [scoped follow-up](evidence/2026-10-07-hs-cell-runtime.json);
engine integration and clean runtime acceptance remain open.

An isolated [actual scalar engine subset](evidence/2026-10-07-scalar-subset.md)
passes 128 host checks at offsets 0-7. Its standalone Wii executable
[ran all 128 checks](evidence/2026-10-07-scalar-wii.md) in stock Dolphin;
16 wire-byte checks fail, measuring the endian incompatibility. The other
112 predicates pass, while alignment safety and physical Wii remain unqualified.
The separate host shutdown failure persists.

An opt-in [scalar adapter comparison](evidence/2026-10-07-scalar-candidate.md)
passes the unchanged 128-check fixture plus 208 edge checks on host/PPC, while
reproducing all 16 reference wire failures in the same PPC executable. This is
an isolated diagnostic result; production packet/structure integration and
clean runtime acceptance remain open.

The [actual packet dispatch comparison](evidence/2026-10-07-packet-dispatch.md)
executes 1,504 checks per implementation. Host reference/candidate each fail
16 older-version consumption/tail checks. PPC reference fails 432; the scalar
adapter retains only those same 16 version failures and matches all 96 wire
goldens. Packet qualification and verifier/version policy remain open; this
does not establish production integration or clean host shutdown.

A third [diagnostic placeholder decoder](evidence/2026-10-07-packet-version-policy.md)
passes 4,416 host/PPC checks while preserving the previous implementations and
their failed assertions. This proves the listed diagnostic version policy;
excluded arrays, verifier safety, production compatibility, clean host shutdown
and physical Wii remain unqualified.

A separately named [bounded verifier diagnostic](evidence/2026-10-07-packet-verifier.md)
passes 73 cases / 383 host/PPC predicates, with exact report equality. It
revalidates cached flat definitions, checks signed native extents, assigns zero
to own-version excluded fields and commits metadata only after full success.
The original verifier and earlier results are preserved. Arrays reject without
child traversal; recursive schema/reserve handling, runtime-version native
capacity, production integration and clean host shutdown remain open.

An isolated [paired array diagnostic](evidence/2026-10-07-packet-arrays.md)
passes 342,620 predicates in 306 case executions on host/PPC, including maximum
representable schema depth, child skipping and stable native-capacity policy.
Original bodies, metadata and all earlier reports/failures remain intact.
Production integration, actual group boundaries, broader ABI, host teardown and
physical Wii remain open; no gameplay or network-session pass is established.

The isolated [actual group comparison](evidence/2026-10-07-packet-groups.md)
passes 1688 host / 1682 PPC predicates, with candidate report agreement and
measured original char-signedness differences. All prior reports remain intact.
Two runs of this DOL exit naturally with host status 0; earlier shutdown faults
remain preserved and general recovery is unqualified. HWI-005/006 stay blocked.
Next gate covers caller size unions, dispatch identity/native capacity and outer
framing before any production proposal.

The [caller framing diagnostic](evidence/2026-10-07-packet-callers.md) passes
4886 host/PPC predicates in 680 reported cases. Original PPC union/member and
unconditional-header-swap behavior is measured separately; candidate reports
and every earlier section agree within their stated scopes. Two separate runs
of this DOL finish the guest report then exit with host fault 0xc0000409;
general recovery remains unqualified.
HWI-005/006 stay blocked. Actual native ABI layouts, dispatch association,
snapshot/cache lifetime and peer wire identity precede production integration.

The [actual native ABI diagnostic](evidence/2026-10-07-native-packet-abi.md)
completes 420 cases / 3387 predicates on host/PPC with zero diagnostic failures,
but compatibility qualification remains BLOCKED (seven host / nine PPC gaps).
Actual catalog identity, receiver size, unsupported raw owner, default wchar
and plain-char semantics gaps are preserved. Every prior report stays exact.
One native-DOL run exits naturally host0; earlier faults remain and general
recovery is unqualified. HWI-005/006 remain blocked. Owned data intake is the
next separate gate; no production, peer, hardware or gameplay pass follows.

The [owned-image inventory](evidence/2026-10-07-owned-xiso-intake.md) now passes
read-only filesystem and 24 Xbox version-5 header checks. It finds 50 files,
five directories and eight movies (including three Xbox demo videos); source
build lookup is NTSC. No assets were extracted. Compressed bodies, tags/BSPs,
conversion, canonical dump authenticity and gameplay remain unqualified.
The next gate prepares a 32-bit Linux validator and private staging; HWI-005/006
remain blocked and HWI-007/008 remain incomplete.

The [existing Linux cache validator](evidence/2026-10-07-owned-cache-validation.md)
now passes strict checks on all 24 owned maps with zero corrections/refusals.
Private staging/read-back hashes and decompression/tag/BSP checks are recorded.
Canonical checksums/authenticity, full conversion, Wii address/memory/endian
loading, runtime and gameplay remain unqualified. Earlier ABI gaps/shutdown
faults stay preserved; HWI-005/006 remain blocked and HWI-007/008 incomplete.

Owned UI index/address preparation: host and emulated PPC pass 983 actual
instances through eight owned memory round trips, plus 1,916 synthetic cases /
8,437 checks each. A separate MEM2 copy restores its reservation; arena/heap
observations do not qualify full peak or fragmentation. Fixed Xbox regions and
current native capacities remain measured blockers; no full-map residency,
nested conversion or gameplay pass. See [address evidence](evidence/2026-10-07-owned-cache-address.md).

The [bounded owner/streaming diagnostic](evidence/2026-10-07-owned-cache-stream.md)
passes 16 host/PPC raw UI cycles across two placements, preserves full source
reservations, rejects stale handles and three deliberate IO identity errors,
and restores MEM2 bounds. Its maximum charge leaves 1,804,256 bytes in the
measured arena under a provisional reserve. State/sound are placeholders;
controlled interval holes are not system fragmentation, and heap arena size
changes. Full memory strategy, nested conversion, physical Wii and gameplay
remain unqualified; earlier ABI/shutdown failures stay preserved.

The [representative typed widget graph](evidence/2026-10-07-owned-widget-graph.md)
passes 189 cases / 5,521 checks and 16 actual host/PPC lifetimes, serializing
two roots and two records losslessly (2,160 bytes). A separate 485-root extent/
count scan does not qualify the full UI graph. Original reservations remain;
the extra PPC workspace/serialization charge leaves 1,800,624 measured MEM2
bytes under a provisional reserve. MEM2 bounds restore; heap arena size grows.
Independent audit passes 291 checks. This is partial named-field conversion;
BSP/resources, engine memory strategy, widget execution, hardware and gameplay
remain open, along with prior ABI/shutdown failures and ticket blockers.

The [exact original probe closure retest](evidence/2026-10-07-original-probe-closure.md)
reproduces its original build/DOL hashes, then two cold stock-profile launches
both pass guest video/timer/arena/input/SD checks and sentinel 0 -> 1 -> 2.
Both naturally exit with host `0xc0000409`; HWI-005 remains BLOCKED on clean
process exit. Later scoped diagnostic clean exits remain valid; general
recovery is unqualified. Next shutdown work is matching-symbol callback-owner
diagnosis, separate from active asset/address/BSP preparation.

The [selected BSP residency diagnostic](evidence/2026-10-07-owned-bsp-residency.md)
passes 75 cases / 5,432 checks and 16 actual host/PPC parent cycles with
32 child loads. Two readable windows exclude the 21,424,380-byte gap;
parent validity is checked before child control access. The actual root is
opaque and descriptor counts are zero; positive descriptor tests are authored.
Original capacities remain, with 1,796,360 charged PPC MEM2 bytes remaining.
Both exact-DOL Dolphin launches pass guest checks then fault naturally with
host 0xc0000409. Earlier failures remain; engine strategy, geometry, hardware,
gameplay and ticket blockers are unchanged.

The [bounded campaign BSP inspection](evidence/2026-10-07-campaign-bsp-inspection.md)
finds 34/34 actual descriptors and nonempty selected collision, surface,
lightmap and cluster blocks in an authorized fallback sample. This is read-only
extent/count inspection; nested fields and geometry remain unqualified.
Duplicate full staging/serialization would exceed the measured PPC remainder;
next preparation uses bounded projection with the existing tag reservation.
No new runtime launch or SD write occurs, and ticket blockers remain.

The [retained campaign material metadata](evidence/2026-10-07-campaign-material-metadata.md)
checks all 34 materials, 68 descriptor relationships and 473,216 bytes of
count-derived environment payload extents. All lightmap vertex counts are zero.
At that checkpoint, shader association and vertex contents were unqualified,
and a separately charged partial native projection was proposed. That checkpoint
involved no campaign decoder/runtime, new extraction, SD write or target launch.

The [fresh campaign tag identity prerequisite](evidence/2026-10-07-campaign-tag-identity.md)
now checks all 34 selected shader references against newly derived raw tags.
Full datum, primary group, shader ancestry and bounded names pass, with 7,797
independent checks and zero discrepancies. This advances metadata preparation;
shader bodies, native target execution, geometry and ticket blockers remain open.
No additional target launch or SD write occurs.

The [partial material API gate](evidence/2026-10-07-material-api.md) implements
transactional root/lightmap/material projections and parent-first generations.
Authored host 64 cases / 48,752 checks and 46 Python tests pass; clean strict PPC
compilation passes, without PPC execution. The actual twelve-slot plan leaves
1,782,032 conservative PPC arena bytes while retaining original reservations.
Retained campaign Python metadata matches 34 materials and the 9,416-byte selected
concatenation. Owned C target execution, engine pools, geometry/GX, gameplay and
ticket acceptance remain open; no additional SD write or target launch occurs.

The [selected campaign material stream](evidence/2026-10-07-material-stream.md) passes 64 material
cases / 48,752 authored checks and 16 owned host/emulated PPC parent cycles with
32 material loads. All 34 selected materials serialize 9,416 bytes with matching
CRC and full SHA; original reservations remain, leaving 1,782,032 PPC arena
bytes after the provisional reserve is charged. State/sound are placeholders
and widget storage is reservation only. Identical guest result-zero reports
precede natural Dolphin host 0xc0000409 faults at 59.866 and independently
59.6373275 seconds. Matching heap counters and restored bounds do not qualify
system peak, engine strategy, geometry/GX, gameplay, physical Wii or clear prior
shutdown/ABI blockers.

The [offline shutdown symbol gate](evidence/2026-10-07-shutdown-symbol-scope.md)
confirms exact stock PE GUID/age and qualifies generic DIA resolution with
17 Python tests, 55 authored PDB cases / 292 checks and strict MSVC builds.
Official routes checked expose no exact PDB acquisition route. Static binary
review links initialization to onexit registration and corrects a retained
mid-instruction disassembly; source/thread ownership and shutdown cause remain
unresolved. No new Dolphin/SD/settings/asset operation occurs; ticket states
and prior runtime/hardware limits remain unchanged.


The [authored triangle word accessor](evidence/2026-10-07-surface-api.md) passes
80 host cases / 50,484 checks; eight strict PPC units compile without execution.
Whole-root ordinals expose bounded unsigned LE16 triples with parent-first
lifetimes and atomic output. Workspace growth adds 24 payload / 64 charged bytes;
the twelve-slot PPC numeric model retains 1,781,968 bytes after its reserve.
This model does not repeat target memory or owned-byte qualification. Vertex
semantics, geometry/GX, gameplay, hardware and ticket blockers remain open.


The [original probe debugger setup attempt](evidence/2026-10-08-shutdown-debugger-setup.md)
stops before watchpoint/lifecycle capture because its controller compares
raw padding/zero-filled data against GDB displayed extents. One early library
stop,0 lifecycle events and explicit owned-process forced cleanup are retained.
The SD backup/source/after, sentinel/log, profile/configs and exact inputs agree.
This setup failure does not establish debugger incapability or shutdown cause;
HWI-005/006 blockers and prior runtime/hardware limits remain open.


The [original probe startup watchpoint capture](evidence/2026-10-08-shutdown-startup-capture.md)
qualifies hardware placement and sees startup reset/accessor followed by word
0 -> 32,348. A bounded writer instruction review supports the observed store,
without source-owner or last shutdown-writer proof. Sixty exception stops consume
the64-stop cap before shutdown; cleanup is forced, with no new guest report.
SD/config/input identities remain; host session/play-time metadata changes are
retained. Original shutdown/ABI and physical-Wii qualification remain open.
