# Material-local surface accessor

`cache_material_get_material_surface` retrieves the selected material's local
triangle record through the existing validated material and whole-root getters.
It checks the local range before adding the signed, publication-validated first
surface, then returns the same three unsigned LE16 words. A local one-past
ordinal rejects even when its corresponding root record exists. Parent-first
lifetimes, disjoint outputs and atomic rejection remain in force.
[Numeric evidence](2026-10-08-material-local-surface-api.json)

Clean source `7618ff9f9fc1204623a655c41fcbb8c4d75a86b0` passes authored host
execution: 96 cases / 52,548 checks, zero failures. The address prerequisite
retains 1,916 cases / 8,437 checks, zero failures. Host build
`70c1d8dc6b2bcf85` and PPC build `28d6a0a8ef80f0d5` each pass eight strict C11
units and ten commands. PPC compile/link/elf2dol succeeds without execution.

Sixteen new cases cover two placements and source offsets zero through seven,
nonzero starts, local/material one-past and SIZE_MAX, empty-at-root-end and
overlapping/out-of-order ranges, unsigned extremes, aliases/canaries and stale
parent/child lifetimes. The maximum 131,072-record case also checks material
composition and the last material's root-end range. Authored range edits unload
both views before changing source. Manual source review passes 28 predicates;
an inherited collect-mode fixture unload prerequisite was fixed before testing,
with its earlier finding retained.

No allocation, layout, workspace, reservation or serialization growth occurs.
Root/material/lightmap/surface projections remain 36/128/20/6 bytes. The
two-lightmap, 34-material workspace remains 8,856 bytes. Earlier selected owned
serialization was 9,416 bytes and is not re-executed here. Compiler-reported
own static frames for this accessor are 208 host bytes and 160 PPC bytes;
those figures do not measure combined call-stack peak or fragmentation.

This gate does not infer vertex-index origin, per-material vertex bounds,
topology, native vertex conversion or GX. It performs no owned-data, SD,
emulator, planner or target-memory run. The [fatal shutdown chronology](2026-10-08-shutdown-fatal-capture.md)
and original ticket blockers remain. HWI-005/006 remain BLOCKED, HWI-007/008 TODO;
engine/gameplay and physical Wii acceptance remain open.
