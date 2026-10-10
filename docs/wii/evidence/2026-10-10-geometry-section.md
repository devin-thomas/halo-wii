# Selected geometry section (HWI-008A)

One real Halo environment section is selected and validated for the first
geometry diagnostic. Selection reads the existing private derivation from the
owned, strictly validated campaign map: BSP ordinal 12, 1,288,192 bytes, SHA-256
`7f7b4e77…`. Nothing new is extracted. [Numeric evidence](2026-10-10-geometry-section.json)

## Selection

Rule: the largest environment-shader (`senv`) compressed material in lightmap 0,
by triangle count. That is global material 3:

| Measure | Value |
|---|---|
| Triangles (surfaces 2,340–5,329) | 2,990 |
| Environment vertices | 5,863 (all referenced) |
| Source records | 187,616 bytes (32 bytes per vertex) |
| Native F32 positions + U16 indices | 70,356 + 17,940 = 88,296 bytes |
| Degenerate triangles | 0 |
| Non-finite, subnormal or negative-zero position words | 0 |
| Section SHA-256 | `79cb1e25375dcac92544a0269012b03707ec1c98ecfedbf69757855d59627565` |

Every triangle index is below the material's vertex count. Every position the
renderer will read satisfies the [HWI-006A boundary](2026-10-10-surface-positions-abi.md).
Two independent selections produced byte-identical manifests.

## Tool

`tools/wii/select_geometry_section.py` reuses the bounded material inspection,
then validates the selected section and writes a private numeric manifest. The
manifest holds counts, extents and hashes; indices, positions, names and raw
bytes are never exported. Seven authored tests cover:
- rule choice;
- repeat determinism;
- index-hash sensitivity to a single changed corner;
- an out-of-range index;
- positive and negative infinity, quiet NaN and signalling NaN;
- uncompressed and unknown materials.

## Input plan

The guest reads the existing private BSP and tag sidecars from the emulated SD.
That route was proven by the 2026-10-07 material-stream run. The guest walks the
section with `cache_material_get_surface_positions`. No converted asset file is
produced and no game bytes enter Git.

## Not established

- Guest residency and memory (HWI-007A), and rendering (HWI-016A).
- Full asset conversion, textures, audio and saves (HWI-008).
- Physical Wii.
