# Compressed environment vertex word accessor

`cache_material_get_compressed_vertex` retrieves one 32-byte environment record
from the selected material's compressed tag-data address. Its ordinal is below
the validated environment count, and the complete environment span resolves
inside the BSP window before reading. The getter returns three position bit
words, packed normal/binormal/tangent words and two texture-coordinate bit words
using explicit LE32 loads and one staged output assignment. Hardware descriptor
Data, buffer offset and base address do not choose this source.
[Numeric evidence](2026-10-08-compressed-vertex-api.json)

Clean source `f476317f096d11144256da13a2958ffca178ef10` passes authored host execution:
112 material cases / 54,459 checks, zero failures.
The address prerequisite retains 1,916 cases / 8,437 checks, zero failures.
Host build `d161fe8d2ea39019` and PPC build `8bd6ed45371ae712` each pass eight
strict C11 units and ten commands. PPC compile/link/elf2dol passes without
execution. Manual source review passes 37 predicates; saved-build review passes
371 predicates. Neither review adds target execution.

Authored cases cover two placements and source offsets zero through seven,
distinct stored words including negative-zero and NaN bits, ordinal/material
boundaries, empty environment data, truncated spans, aliases/canaries and
stale child/outer-parent lifetimes. Source remains immutable while published;
failed setup prerequisites abort before backing is overwritten.

No allocation, persistent projection, workspace, reservation or serialization
growth occurs. The 32-byte result is caller storage. Root/material/lightmap and
surface projections remain 36/128/20/6 bytes, and the prior two-lightmap,
34-material workspace is 8,856 bytes. Earlier 9,416-byte owned serialization is
not re-executed. Compiler-reported own static frames are 480
host bytes and 352 PPC bytes, not combined call-stack peak.

Float bits and packed vectors remain uninterpreted. This gate does not establish
triangle-to-vertex origin, decompression, native geometry, GX, engine/gameplay or
physical Wii acceptance. No owned-input, planner, emulator, SD, profile or target
execution occurs. [Shutdown evidence](2026-10-08-shutdown-symbol-build.md) remains
blocked. HWI-005/006 remain BLOCKED; HWI-007/008 remain TODO.
