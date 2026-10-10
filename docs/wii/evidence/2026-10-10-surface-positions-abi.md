# Surface position boundary on PPC (HWI-006A)

This qualifies only the data the first real-geometry diagnostic reads: one
material-local triangle's indices and its three vertex positions.
[Numeric evidence](2026-10-10-surface-positions-abi.json)

## Existing decoders, first PPC execution

The authored material fixtures had run on host, with PPC compiled only. At
`c1f703af` they now run on PPC in stock Wii-mode Dolphin, build
`933ff7c55fb63969`: 133 cases / 91,506 checks / 0 failures, plus 1,916 address
cases / 8,437 checks / 0 failures. Host at the same commit gives the same
counts. This includes the packed-vector float-bit goldens and all 5,120
component codes, so their PPC runtime agreement is now executed rather than
inferred. The reported material-control size differs (88 bytes on host, 80 on
PPC) because of pointer width; no check differs.

## New boundary

`cache_material_get_surface_positions` returns a triangle's three positions as
nine native binary32 bit words in corner order:

| Input | Contract |
|---|---|
| Surface | Material-local ordinal, bounded by the material's surface range |
| Corner indices | Three unsigned LE16 values, each below the material's environment vertex count before any payload read |
| Positions | Three LE32 binary32 bit patterns per corner from the 32-byte compressed record |
| Infinity / NaN | Rejected with `CACHE_MATERIAL_VALUE` at `corner * 3 + axis`; output untouched |
| Signed zero / subnormal | Preserved bit-exact; written to GX arrays with integer stores |

Normals, tangents, texture coordinates and lightmap records are outside this
boundary and are not advertised as supported.

Clean source `9176d8d158fd`: host build `e402f319d9a92663` and PPC build
`ae64243c9aa855e9` (DOL `53630ec7…ee44`) both pass 149 cases / 93,378 checks / 0
failures. The PPC result comes from execution in stock Wii-mode Dolphin, with
host exit 0x0 and no forced stop. The 16 new cases cover:
- signed zeros, subnormal extremes, the smallest normal and the largest finite
  values;
- positive and negative infinity, signalling NaN and negative quiet NaN;
- an out-of-range corner index, range bounds, output aliasing, null arguments,
  and stale or unloaded views;
- two placements and eight byte offsets.

With the rejection temporarily disabled, the new rejection checks failed.

## Not established

- Agreement with original Xbox/MSVC execution. Results match the authored oracle
  only.
- GP handling of subnormal positions, and any rendering of real geometry
  (HWI-016A).
- Selection of owned map data (HWI-008A), residency (HWI-007A) and physical Wii.
- HWI-006's broader script, save, calling, intrinsic, network and gameplay-math
  obligations.
