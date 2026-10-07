# Test and evidence contract

## Environments are independent

Record compile, Dolphin and physical-Wii results separately for the exact build and scenario. Allowed states are `untested`, `pass`, `fail`, `blocked`. A compact badge maps pass to ✅ and all no-current-pass states to ❌, with a reason beside it. A CI compile result is never a gameplay pass.

A passing record includes a public commit, upstream commit, binary SHA-256, toolchain, test ID, data/profile fingerprint, emulator or device configuration, observed result and durable evidence reference. Do not expose local addresses, user paths, console keys or proprietary data in public records.

## Minimum progression

| Capability | Verification |
|---|---|
| Probe | Visible new build ID, display, inputs, timer/arenas, clean exit |
| Combat | Move/aim/fire/melee/grenade/interact, HUD/audio, shootable actors |
| Campaign | Every level, original encounters/scripts, vehicles, checkpoints, transitions and endings |
| Persistence | Save/load after full application exit and cold boot, interrupted/corrupt/versioned saves |
| Display | Calibration shapes, HUD, weapon view and cinematics at every aspect; split-view projection |
| Wii input | PAD, Classic, real IR/gesture behavior, lost tracking and reconnects |
| Local multiplayer | Full 2P campaign; 2/3/4-player competition with independent inputs |
| LAN | Host/join/round transitions; multiple locals per machine; original 16-player/four-machine target |
| Cross-port | Compatible upstream Windows, then Linux/Android, with matched protocol/capacity evidence |
| Performance | Frame/tick/CPU/GPU/memory measurements without changing simulation or competitive cues |

Use synthetic fixtures for public CI. Owned game data stays in controlled local testing. Never run untrusted public PR code automatically on a personal runner that can read dumps, credentials or the local network.

## Hardware 1.0 gate

Public source and early releases may be Dolphin-only. Wii 1.0 requires real-hardware qualification of the Phase 1–4 feature set, including motion, campaign co-op, four-player competition and LAN. Record topology honestly: one physical Wii plus Dolphin peers is not evidence of four physical Wiis.

Cross-port work is a required post-1.0 milestone, not an optional idea. The port cannot claim complete interoperability merely because it reused a source file or carries the same version number.

## Simulation invariants

Rendering may become cheaper. AI semantics, physics, collision/hit resolution, timers, scripts, authoritative objects, RNG and rules may not be reduced for Wii performance. An original Xbox quirk requires evidence, not a resemblance. Check that supposedly cosmetic changes do not consume gameplay RNG differently or remove a tactically meaningful effect. Missing a render target keeps the issue open; it is not permission to discard simulation ticks.

## Updating status

Edit [status.json](status.json) and [STATUS.md](STATUS.md) together. Passes require evidence. Invalidate or explicitly scope earlier passes when code, toolchain, assets or configuration materially change. Keep failed and blocked cases visible.
