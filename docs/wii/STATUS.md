# Wii port status

**Probe compile pass (2026-10-07):** the asset-free native probe builds with
configure.py/Ninja and official devkitPPC/libogc. Clean, incremental and checkout
paths containing spaces passed. See [compile evidence](evidence/2026-10-07-probe-build.md)
and [build setup](BUILDING.md). Dolphin and physical Wii remain separate gates.

| Scope | Compile | Dolphin | Physical Wii |
|---|---|---|---|
| Asset-free Wii diagnostic | Passed: asset-free probe | Untested | Untested |
| Halo combat slice | Untested | Untested | Untested |
| Complete campaign | Untested | Untested | Untested |
| IR / motion | Untested | Untested | Untested |
| 2P campaign co-op | Untested | Untested | Untested |
| 4P competitive split-screen | Untested | Untested | Untested |
| Wii LAN | Untested | Untested | Untested |
| OpenCE mixed-port play | Untested | Untested | Untested |

There is no current pass for either execution environment. `Dolphin: ✅ | Hardware: ❌` is an acceptable **future** compact status once a specified build/scenario really passes Dolphin and has no current hardware pass. Underlying records distinguish untested, failed and blocked.

Physical Wii validation gates 1.0, not source publication or alpha development. Wii 1.0 does not close the later required cross-port milestone. See [test policy](TESTING.md) and machine-readable [status](status.json).
