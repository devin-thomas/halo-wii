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
