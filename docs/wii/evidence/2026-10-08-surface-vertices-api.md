# Explicit material triangle vertex association

`cache_material_get_surface_vertices` selects a material-local surface, checks
all three unsigned LE16 indices against that material's environment count, then
retrieves three compressed environment records. The source CPU triangle path
uses these indices directly as zero-based ordinals in the selected material's
compressed tag data. All 24 words are staged before the 96-byte caller output
is published. Repeated and reversed indices preserve stored corner order.
[Numeric evidence](2026-10-08-surface-vertices-api.json)

The source rule is supported by [the triangle consumer](https://github.com/devin-thomas/halo-wii/blob/d789e28e1fb6a9f3b08ffb12a64efadbe538f7b6/source/structures/structures.c#L592)
and [the selected environment count checks](https://github.com/devin-thomas/halo-wii/blob/d789e28e1fb6a9f3b08ffb12a64efadbe538f7b6/port/linux/game/tag_schema_collision.c#L1511).
Source-origin review passes 20 predicates and implementation/fixture review 36.
No hardware buffer offset/base adjustment or automatic global owner lookup is
introduced. Overlapping and out-of-order ranges retain explicit caller selection.

Clean source `d789e28e1fb6a9f3b08ffb12a64efadbe538f7b6` passes authored host execution:
128 material cases / 64,417 checks, zero failures, plus 1,916 address cases /
8,437 checks, zero failures. Host build `edf3832c7325ddfd` and PPC build
`3386c8f15e023829` each pass eight strict C11 units and ten commands.
PPC compile/link/elf2dol passes without execution. Saved-build review passes
377 predicates without introducing new runtime evidence.

Sixteen new placement/offset cases cover distinct/repeated/reversed corners,
both materials, different tag-data and hardware Data addresses, invalid later
corners, indices at the count and 65535/32768, empty environment data, full
96-byte aliases/canaries and stale overwritten backing. The 64,000-vertex case
checks direct ordinals 0/32768/63999 and rejects 65535 without partial output.
Failed setup and unload prerequisites abort before source or backing changes.

No allocation, persistent layout, workspace, source reservation or inspection
serialization growth occurs. Root/material/lightmap/surface projections remain
36/128/20/6 bytes; each vertex projection is 32 bytes. The prior two-lightmap,
34-material workspace is 8,856 bytes, and earlier 9,416-byte owned serialization
is not re-executed. Compiler-reported own frames are 544
host bytes and 424 PPC bytes, not combined call-stack peak.

This environment-only association does not apply lightmap-count/bitmap limits,
global material ownership, packed-vector/float decoding, native geometry or GX.
No owned-input, planner, SD, profile, emulator or target execution occurs.
[Shutdown qualification](2026-10-08-shutdown-symbol-build.md), engine/gameplay
and physical Wii remain open. HWI-005/006 stay BLOCKED; HWI-007/008 stay TODO.
