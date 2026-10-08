# Source packed-vector decoding

`cache_material_decode_packed_vector` extracts unsigned 11/11/10-bit fields,
computes each signed field value explicitly and returns three binary32 floats
using `(2*q+1)` times the source float reciprocal (1/2047 or 1/1023).
All 32-bit inputs are valid. The 12-byte caller output is staged before one
assignment; output and result objects must be truthful and disjoint.
[Numeric evidence](2026-10-08-packed-vector-api.json)

The [original decoder](https://github.com/devin-thomas/halo-wii/blob/aa48f7c733577360d02908a80b03ebc9be17d184/source/rasterizer/rasterizer_geometry.c#L516)
uses a biased midpoint: raw zero produces small positive components, all ones
small negative components, and signed extrema reach -1/+1. Explicit field
arithmetic avoids implementation-defined out-of-range unsigned-to-signed
conversion and signed-shift dependencies. No normalization, clamping or encoder
is added. Source audit passes 23 predicates and final implementation/fixture
review 38, with zero failures.

Clean source `aa48f7c733577360d02908a80b03ebc9be17d184` passes authored host execution:
133 material cases / 91,506 checks, zero failures, plus 1,916 address cases /
8,437 checks, zero failures. Host build `2566b20205b2b6a8` and PPC build
`f36901d121735f02` each pass eight strict C11 units and ten commands.
PPC compile/link/elf2dol passes without execution. Saved-build review passes
384 predicates, zero failures.

Five new cases exhaust all 5,120 component codes with mixed other fields,
compare nine fixed float-bit goldens and guard null arguments with complete
12-byte canaries. Existing offset/placement fixtures compose 288 normal,
binormal and tangent words through the raw compressed-vertex getter. All three
component bits are compared against a separately authored source-order oracle
using unsigned shifts, memcpy to int32 and the original float operations.

Measured equality covers that host oracle and the recorded GCC 16.2.0 C11/O2
build without fast-math flags. Static assertions require 4-byte binary32 floats
(radix2, mantissa24, min_exp-125, max_exp128). Dynamic floating environment
modes, original Xbox/MSVC execution and PPC runtime equality remain unmeasured.

No allocation, persistent layout, workspace, source reservation or inspection
serialization growth occurs. Root/material/lightmap/surface projections remain
36/128/20/6 bytes; each compressed vertex projection is 32 bytes. Prior workspace
8,856 bytes and earlier owned serialization 9,416 bytes are unchanged and are
not re-executed. Compiler-reported decoder own frames are
32 host bytes and 40 PPC bytes,
not combined call-stack peak.

Position/texcoord float interpretation, native geometry, GX, shutdown,
engine/gameplay and physical Wii acceptance remain open. No owned-input,
planner, SD, profile, emulator or target execution occurs.
HWI-005/006 stay BLOCKED; HWI-007/008 stay TODO.
