# Halo CE · Wii port

**State: source workspace established; toolchain qualification in progress.**
The read-only toolchain preflight is implemented. No Wii game binary is available,
and no Dolphin or physical-Wii pass is claimed.

This public fork is intended to port OpenCE to Wii using native devkitPPC/libogc/GX backends. Original Halo gameplay and content remain the target. A public alpha can be incomplete; the roadmap cannot silently remove missing systems to claim completion.

## Navigation

[Build](BUILDING.md) · [Run](RUNNING.md) · [Test](TESTING.md) · [CI](CI.md) · [Compatibility](COMPATIBILITY.md) · [Status](STATUS.md) · [Data and licenses](DATA-AND-LICENSES.md) · [Upstream baseline](UPSTREAM.md)

## Delivery sequence

| Stage | Required outcome |
|---|---|
| Foundation | Asset-free Wii probe, toolchain, address/ABI/endian and memory feasibility |
| 1 · Combat | Controller-driven Halo combat slice with shootable actors |
| 2 · Campaign | Complete campaign runtime, AI, scripting, vehicles, checkpoints and cinematics |
| 3 · Wii-native | Classic pad, mandatory IR/motion controls, flexible aspects and presentation fidelity |
| 4 · Wii 1.0 | Two-player campaign co-op, four-player competitive split-screen, original LAN scope and physical-Wii qualification |
| 5 · Interoperability | Required mixed-platform OpenCE sessions; Internet feasibility work |

Wii is primary. GameCube is later research rather than a constraint. Network campaign, binary Xbox save compatibility and retail-Xbox wire compatibility are stretch work. Multiplayer bots are development tools, not required shipped content.

## Non-negotiable behavior

Preserve the original 30 Hz gameplay semantics. Target stable 30 FPS with presentation-only optimization. Do not thin AI, change physics, remove gameplay objects or modify game rules to recover frame time. A render optimization must also preserve visibility and cues that affect play. IR/motion is mandatory for Wii 1.0, not required for the first playable alpha.

Display modes separate actual video output from the rendered view: Auto, 4:3, 5:4, 3:2, 16:9, 1:1 and Custom are planned. No arbitrary native framebuffer or CRT-resolution support is claimed. Dolphin's output settings remain separate.

## Source reference

The initial planning audit inspected `2b0327bc80ca38c90894cb56b19652cbe85733ff`.
The [current pinned integration baseline](UPSTREAM.md) is
`4e8ed2f196e0edd1f2830a4de9841686aabbf466`; configure.py still has
Linux/Windows/Android generators, **not a Wii generator**. Source inspection is
not evidence of a working port.

Contributors must be able to build and test documented public capabilities without access to any private planning repository. Preserve upstream documentation and license notices. Do not place proprietary game data, Nintendo SDK code, console keys or personal device information in the fork.
