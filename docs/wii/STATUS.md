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

No clean Dolphin or physical Wii pass exists. Guest observations and the host-teardown failure are recorded in [Dolphin evidence](evidence/2026-10-07-dolphin-probe.md). Synthetic C host/PPC results are [separate](evidence/2026-10-07-abi-fixtures.md). `Dolphin: ✅ | Hardware: ❌` is an acceptable **future** compact status once a specified build/scenario really passes Dolphin and has no current hardware pass. Underlying records distinguish untested, failed and blocked.

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
