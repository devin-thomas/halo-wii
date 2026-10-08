# Partial material API compile gate

Clean source `840c64007bbfaf0428d96daa248471d46e3669dc` implements an allocation-free
partial Xbox root/lightmap/material projection and the generic numeric inspector.
The authored host fixture passes 64 cases / 48,752 checks with zero failures or
aborts. All eight units compile strictly for PPC and link into an asset-free DOL.
The DOL has not been executed. [Numeric evidence](2026-10-07-material-api.json)
identifies sources, builds, artifact hashes, memory measurements and limits.

The API measures actual aggregate requirements, publishes transactionally into
two caller-owned workspace halves, exposes copied projections, serializes selected
spans, and unloads the child generation. Its view copies the arena/BSP parent pin
and checks it before accessing material control/workspace. The fixture overwrites
released parent backing, including both child controls, then tests old handles
before and after same-address rebind. Failed binding preserves the prior control,
view, publication and source; scratch may change. Active unload advances generation,
inactive unload is idempotent, and exhaustion rejects atomically.

Counts retain source limits: 128 lightmaps, 2,048 materials per lightmap and 64,000
vertices per material. Root surfaces count/span are checked even for empty graphs.
Tag-data maxima are 4,864,000 uncompressed and 2,560,000 compressed bytes. Shader
NONE skips meaningless reference words; other references require full salted datum,
matching primary group, shader primary/ancestor and tag-window NUL names. Name
length stays opaque. Material surface ranges are signed and in bounds; this partial
probe does not impose a global ordering requirement.

Non-null hardware words identify proper-kind 12-byte descriptor records. Their
Data extents use type strides 56/32/20/8. Xbox compressed tag-data minimum remains
32N + 8M, independently of those hardware strides. NULL hardware establishes no
payload extent; empty counts preserve words without following payloads. These
checks qualify resource metadata, without interpreting vertex/triangle contents.

Serialization copies the complete 648-byte root, complete lightmap table and each
material table in lightmap order. Known numeric words are explicitly LE; signed
values and float bits survive, opaque bytes and encoded addresses remain. This is
selected inspection concatenation, not a relocated cache or durable save. Buffers
and controls require truthful, disjoint, immutable single-caller ownership. Unload
requires accessible control and cannot be used on freed parent backing.

## Recorded validation

- Clean host build `58e3199e84412960`: execution passes, including the existing
  1,916 address cases / 8,437 checks and the material cases above.
- Clean PPC build `3d3fac94df8c1fce`: eight strict C11/O2/Wall/Wextra/Werror/stack-usage
  compiles, link and ELF-to-DOL conversion all exit zero. DOL SHA-256 is
  `c85c357aaa4d539a60a8ba992e2ac15fa35466571ca9f11a2c9d4c3ace031119`.
- Python material 46 tests, BSP regression 36 tests and widget regression 30 tests
  pass. Seven incompatible material runner modes reject before generating output.
- Independent C/source review is clear. Supplemental authored host checks pass
  exact DATA maxima and excess, rejected output preservation, inactive exhausted
  epoch and stale queries after freeing outer backing. No owned assets are used
  by C fixtures or generated binaries.

Dirty-source integration preflights at `77aacc70` also passed, with build IDs
`c049903ff9d729b1` / `6349feea153df133`. They are separate from the clean final
builds. Full source snapshots and commands are retained for both integration
phases. Earlier inspector test harness failures are retained: a narrow overflow
error regex, then a Windows-denied open-FD pathname replacement. Corrected current
tests pass; superseded test source snapshots are absent, so this is not a replay
of those historical sources. The independent supplemental harness initially
failed setup with an unrounded BSP allocation; its initial binary and commands
are retained, while its initial source was superseded. A corrected authored
512-byte-rounded fixture passes. Coordinator reruns of the retained binaries
confirm initial exit one at setup and corrected exit zero with 27 supplemental
checks; they do not reproduce a compilation of the superseded initial source.
No production compiler failure occurred in this material gate.

## Retained campaign and charged memory

The generic Python inspector reads the fresh private tags plus retained campaign
BSP at the clean source. All 34 materials / 14,788 environment vertices / zero
lightmap vertices pass metadata checks. Selected concatenation is 9,416 bytes,
CRC32 2,135,278,324, SHA-256
`e58ffaeb6e7e82553d1d770a6e21e585d0246f3a0febb3dbba8ec3169e614022`.
Input hashes remain unchanged. This is Python metadata inspection, without owned
C execution, target serialization, additional map extraction, SD writes or launch.

Actual API structs measure material/root/lightmap 128/24/20 bytes on both ABIs;
control is host 88 / PPC 80 and view 56. PPC ELF symbol measurements are compile
observations, not target execution. Actual-header plan inputs were frozen at dirty
`77aacc70`; their hashes match the clean final build. Native two-half workspace is
8,832 bytes and selected serialization 9,416. The twelve 64-byte-aligned slots retain
22 MiB tags, 20 MiB state, 4 MiB sound, 65,535 index records, two 64 KiB IO buffers,
conservative widget storage, both native controls and the provisional 2 MiB reserve.
They replace the earlier duplicate 2,048-byte BSP staging/serialization slots.

The PPC numeric first placement requires 52,582,096 bytes. Alternate placement
charges its additional 4,096 bytes against the original 54,368,224-byte arena,
giving maximum charge 52,586,192 and minimum remaining 1,782,032. Host allocated
placement requires 52,582,744, with 1,785,480 remaining, dependent on alignment.
Numeric PPC addresses are arithmetic scenarios, without accessible-storage proof.
Per-function stack records are retained; caller view/control observations do not
qualify full call-chain or system peak. State/sound are canary placeholders;
textures, initialized engine pools, geometry/GX, hardware and gameplay remain open.

Independent actual-plan/build/publication consistency passes 753 checks with
zero discrepancies. It parses the PPC ELF's 16 size and 20 offset symbols,
checks all twelve slots, verifies clean-build/source/artifact hashes and reviews
the numeric publication/privacy scope. Audit SHA-256 is
`36bc56d030987aecfd500386987c0393de51c654b7e57478db90f1c99ca25aed`.
This audit uses retained numeric inspection/command evidence, without another
owned-byte read, compiler/planner run or target launch.

HWI-005/006 remain BLOCKED and HWI-007/008 remain TODO. The original Dolphin host
exit fault remains unchanged. A bounded campaign runtime requires its own reviewed
integration, clean builds and separately authorized staging/launch checkpoint.
