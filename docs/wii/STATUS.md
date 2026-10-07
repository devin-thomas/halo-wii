# Wii port status

**Source bring-up (2026-10-07):** repository setup, pinned upstream inventory and
read-only toolchain preflight are available. The preflight's three synthetic
failure tests pass. These host checks do not establish a Wii compilation pass.
The current development environment lacks devkitPPC/libogc; the native probe
and Wii generator remain unimplemented. See [build setup](BUILDING.md).

| Scope | Compile | Dolphin | Physical Wii |
|---|---|---|---|
| Asset-free Wii diagnostic | Blocked: missing toolchain and implementation | Untested | Untested |
| Halo combat slice | Untested | Untested | Untested |
| Complete campaign | Untested | Untested | Untested |
| IR / motion | Untested | Untested | Untested |
| 2P campaign co-op | Untested | Untested | Untested |
| 4P competitive split-screen | Untested | Untested | Untested |
| Wii LAN | Untested | Untested | Untested |
| OpenCE mixed-port play | Untested | Untested | Untested |

There is no current pass for either execution environment. `Dolphin: ✅ | Hardware: ❌` is an acceptable **future** compact status once a specified build/scenario really passes Dolphin and has no current hardware pass. Underlying records distinguish untested, failed and blocked.

Physical Wii validation gates 1.0, not source publication or alpha development. Wii 1.0 does not close the later required cross-port milestone. See [test policy](TESTING.md) and machine-readable [status](status.json).
