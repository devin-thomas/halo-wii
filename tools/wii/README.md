# Wii development helper

## Offline Windows shutdown symbols

Read the executable's PE CodeView identity without loading it:

```powershell
python -B tools/wii/inspect_pe_symbols.py '<local-executable.exe>' --expected-sha256 '<64-lowercase-hex-digits>'
python -B -m unittest discover -s tools/wii -p test_inspect_pe_symbols.py
```

The bounded reader requires one RSDS record and reports its GUID, age and PDB
basename, plus file and record hashes. It omits the embedded build-machine PDB
path. Same-API stat guards and an optional required full hash protect the read
snapshot; they do not exclude concurrent writers or prove PE loader acceptance.

Build `resolve_shutdown_symbols.cpp` from an x64 MSVC developer shell using
your installed DIA SDK. Keep compiled outputs in an ignored local directory:

```powershell
cl /nologo /std:c++17 /EHsc /W4 /WX /utf-8 /I '<VS>/DIA SDK/include' tools/wii/resolve_shutdown_symbols.cpp /Fo.local/resolver.obj /Fe.local/resolver.exe /link ole32.lib oleaut32.lib
.local/resolver.exe --dia-dll '<absolute-local-DIA-DLL>' --pdb '<absolute-local-PDB>' --guid '<PE-RSDS-GUID>' --age 1 --function-rva 0x00f8e570 --function-rva 0x00f8e5bf --data-rva 0x01f50148
```

Create the output directory first. Use the trusted installed AMD64 DIA DLL;
the resolver loads that DLL directly, without registry setup or symbol-server
lookup. Inputs must use local drive-rooted paths; UNC/device paths and mapped
remote drives are rejected. The executable being investigated is never loaded.
Supply the GUID and age from its exact hashed PE; a PDB mismatch exits before
any RVA result is published. Repeated function/data queries are limited to 64.

JSON distinguishes an exact symbol start, a containing byte extent, an
unqualified nearby symbol and a missing symbol. It preserves the requested
RVA, symbol RVA, signed displacement and DIA displacement, with extent units.
Exit 0 means every query has qualified address coverage; exit 4 retains JSON
for nearby/missing results. Argument errors exit 2, DLL/PDB validation errors
exit 3 and DIA/property/output errors exit 5. Address coverage alone does not
prove callback ownership, thread state or shutdown cause. Keep local PDBs,
compiled binaries and reports containing private paths out of Git.

## Toolchain preflight

Run the read-only preflight from the repository root:

```powershell
python tools/wii/check_toolchain.py --devkitpro C:\devkitPro
python tools/wii/test_check_toolchain.py
```

Use your actual native installation path. The preflight reports missing compiler,
binutils, ELF-to-DOL converter, Wii rules, libogc and Ninja, and rejects a compiler
with a non-PPC target. It exits nonzero if a dependency check fails. It installs
nothing, changes no environment variables and does not compile or run code on Wii.
`--json` prints local tool paths; review the output before publishing it.

## Binary sender

`Send-WiiBuild.ps1` sends an **already-built** DOL/ELF with an installed wiiload executable to an explicitly selected private IPv4 address. It is convenience tooling, not game implementation or an automatic Wii installer.

## Example (replace every example path/address)

```powershell
.\tools\wii\Send-WiiBuild.ps1 `
  -ConsoleAddress '192.168.1.50' `
  -WiiloadPath 'C:\devkitPro\tools\bin\wiiload.exe' `
  -BinaryPath '.\build\wii\probe.dol' `
  -WhatIf
```

The address/path are examples, not discovered values or guaranteed installation locations. Remove `-WhatIf` only after verifying the target console has HBC running with its network receiver ready. The probe path is the planned build artifact; it will not exist until that target is implemented and built.

The helper validates the files/address, hashes the selected binary, sets WIILOAD only for the child invocation and restores the previous environment afterward. It does not modify execution policy, registry, firewall, console firmware or storage. Success of wiiload is not proof of successful device execution; confirm the visible build ID and record a test result.

## Validation boundary

The helper was authored and inspected as part of the planning pack. No PowerShell runtime, Windows wiiload installation or Wii was available for execution during authoring. Validate in Windows first with `-WhatIf` and a small known-good probe. Test missing inputs, invalid addresses, sender failures and WIILOAD restoration before relying on it. It intentionally accepts RFC1918 IPv4 only; no automatic discovery, DNS, public addresses or router forwarding.

Read [running](../../docs/wii/RUNNING.md) for the separate data/log and physical-operator requirements.

## Read-only owned image inventory

```powershell
python tools/wii/xiso_inventory.py '<owned-image>.iso' --output '<external-private-folder>/inventory.json'
python tools/wii/test_xiso_inventory.py
```

The reader opens the image read-only, hashes it and traverses every reachable
XDVDFS directory entry. It rejects cycles, aliases, unsafe/unsupported names,
overlapping or out-of-range allocations and explicit resource-limit violations.
It writes only an explicitly requested new JSON report; existing output paths
are refused. Keep inventory reports outside Git with the owned image.

Exit 0 means filesystem inventory and every observed map's Xbox version-5
header identity passed. Exit 2 means a valid filesystem has no maps or some
map headers are unsupported; the JSON still records those issues. Exit 1 means
structural/input/report failure. The report contains metadata and hashes, no
extracted game bytes. Only printable ASCII XDVDFS entry names are supported;
cache-header strings use a lossless Latin1 presentation.

Header identity does not validate decompression, checksum, tags, geometry,
scripts, asset conversion or gameplay. Stored and declared decompressed lengths
remain separate. The runtime maps-only importer in `port/linux/src/xiso.c`
remains unchanged and does not provide this complete reachable-tree inventory.

## Bounded cache address diagnostics

`inspect_cache_residency.py` streams a supported Xbox v5 compressed map with
64 KiB inflation chunks and retains at most the 22 MiB tag-data bound. It
reports numeric index/BSP metadata and hashes. Optional `--tag-output` writes
private raw tag bytes to a new external path; keep those bytes and reports out
of Git. Arbitrary bytes after the first zlib stream are observed as trailing
content, without canonical padding validation. BSP metadata uses unrounded
file lengths; the existing loader/strict validator remains a separate check.

```powershell
python tools/wii/inspect_cache_residency.py '<private-map>.map' --output '<external-private-folder>/residency.json'
python -m unittest discover -s tools/wii -p test_inspect_cache_residency.py
python tools/wii/run_cache_address.py --cc '<host-gcc>' --output .local/cache-address-host
python tools/wii/run_cache_address.py --cc '<devkitPPC-gcc>' --wii-devkitpro '<devkitPro>' --output .local/cache-address-ppc
```

The default C diagnostic contains authored synthetic data. Explicit
`--private-tag-data '<external-tag-blob>'` embeds owned raw bytes in generated
source and binaries under a new Git-ignored output directory. Never publish
that generated source, ELF/DOL or host executable. The diagnostic decodes
little-endian numeric addresses, datums and index words, then resolves checked
offsets in owned storage. It does not cast the original Xbox address into a
Wii pointer. It preserves parent groups and opaque words; no capacity constants
change. Header/index/name/root-address references are qualified separately from
nested tag bodies, unloaded BSP contents, floats, geometry, scripts, assets,
canonical checksums, conversion and gameplay. Its round trip is a memory
snapshot, not a durable save-file format. Immutable truthful buffers and no
concurrent mutation are required; size/mtime checks are not writer exclusion.

`run_cache_address.py --stream-file 'sd:/<new-private-tag-file>'` selects the
bounded arena/streaming diagnostic. It requires `--private-tag-data` for numeric
CRC/count goldens, and generates no embedded raw byte array. The host executable
receives that external raw file as an argument; Wii opens the separately staged
private SD file read-only. Stage a new file with exclusive creation/read-back
while Dolphin is stopped; preserve prior SD files and profile settings.

The experimental profile requests 22 MiB tags, current 20 MiB state, 4 MiB
sound, 65,535 index records, two synchronous 64 KiB IO buffers and a provisional
2 MiB reserve. State/sound are canary placeholders, not engine pools. The generic
planner charges actual alignment and rejects oversized/fragmented spans; tagged
handles reject old generations after release/rebind. Returned native views are
temporary and must be discarded before release. Alternating buffers do not
provide asynchronous IO. Raw CRC is not a canonical Xbox checksum. IO failures
can leave partial or full tag writes; ownership remains live for explicit cleanup.
Host tmpfile error fixtures are separate from actual PPC SD reads. Controlled
free intervals and heap samples do not establish system fragmentation/full peak.

## Representative typed widget graph

The authored C cases also run without a game file:

```powershell
python tools/wii/run_cache_address.py --cc '<host-gcc>' --output .local/cache-widget-synthetic --widget-fixture
python -m unittest discover -s tools/wii -p test_inspect_widget_graph.py
```

`inspect_widget_graph.py` produces numeric identity goldens from an external
raw tag blob and a caller-selected widget ordinal. It checks a bounded partial
UI layout and traverses the source's loaded-child/column-description edges.
Its output contains no tag names, scripts or raw game bytes; use a new external
output file and preserve the original blob. Workload capacity is separate from
the source's block limits and depth policy.

```powershell
python tools/wii/inspect_widget_graph.py '<external-tag-blob>' --root-ordinal 407 --node-capacity 2 --output '<external-private-folder>/widget-goldens.json'
python tools/wii/run_cache_address.py --cc '<host-gcc>' --output .local/cache-widget-host --private-tag-data '<external-tag-blob>' --stream-file 'sd:/<private-tag-file>' --widget-goldens '<external-private-folder>/widget-goldens.json'
```

For PPC, add the existing compiler/SDK arguments. This explicit option charges
projection and atomic-decode scratch workspace plus inspection serialization
storage in separate aligned arena slots. The prior capacities and provisional
reserve stay intact. The generated header contains numeric graph goldens only;
the actual tag file is streamed read-only. The Wii report uses a separate
`cache-widget.log` and WIDGET build markers.

The allocation-free decoder exposes named numeric fields, signed values,
float bit patterns, references and bounded block elements; unknown byte spans
remain opaque. Views retain their originating owner epoch and reject use after
release/rebind. Lossless inspection serialization copies opaque bytes and writes
named numeric fields explicitly as LE. It preserves original encoded addresses,
so concatenated spans are not a standalone relocated cache or durable save.
This partial representative graph does not execute widgets or qualify complete
UI conversion, resource/BSP loading, final memory strategy, hardware or gameplay.
## Bounded BSP residency diagnostic

Run the authored BSP cases without game files:

```powershell
python tools/wii/run_cache_address.py --cc <host-gcc> --output .local/cache-bsp-synthetic --bsp-fixture
python -m unittest discover -s tools/wii -p test_inspect_bsp_residency.py
```

For a private raw tag blob plus a raw BSP sidecar, generate a new external
numeric inspection record:

```powershell
python tools/wii/inspect_bsp_residency.py <external-tags> <external-BSP> --declared-map-length <inflated-map-bytes> --bsp-ordinal <ordinal> --output <new-external-JSON>
```

The inspector checks the selected scenario/reference, full datum/group,
512-byte rounded residency, 24-byte LE BSP header, opaque 648-byte root extent,
12-byte descriptor tables and one-byte descriptor Data address bounds. It
exposes two valid windows only: actual retained tag bytes and actual declared
BSP bytes. Neither the unread gap nor sector-rounding tail becomes valid data.
Source runtime BSP count is 16; the standalone/PC tool limit 32 remains separate.
Other roots and geometry payload lengths/content are unqualified. Metadata is
numeric/hash-only, inputs are read-only stat guarded, and output publication
is exclusive; these checks are not a filesystem lock.

Add these arguments to a private `--stream-file` build:

```text
--private-bsp-data <external-BSP> --bsp-goldens <external-JSON> --bsp-stream-file sd:/<private-sidecar-path>
```

The raw sidecar is not embedded in the generated header. Host runs take the
two external input paths; PPC reads the two named SD files, which must be
privately staged and identity-checked separately. Optional `--widget-goldens`
keeps the existing representative widget graph in the same diagnostic.
`--bsp-fixture` is asset-free and mutually excludes private/streaming modes.

The streaming diagnostic preserves the full original tag/state/sound/index/IO
reservations and provisional reserve, then separately charges BSP staging,
inspection serialization and native child control slots. BSP bytes occupy
their encoded offset inside the existing tag reservation. The parent epoch
is checked before child control access, allowing that control to live in a
separate parent-owned slot. The second child load remains live until parent
release; both parent and independent child lifetime failures are tested.
Serialization copies the whole declared BSP and explicitly writes known
header/descriptor LE words while preserving original addresses and opaque
root/content bytes. This is an inspection round trip, not a relocated cache,
full typed BSP conversion, engine switching behavior or geometry execution.

## Partial material graph

Run the authored material cases with no game files:

```powershell
python tools/wii/run_cache_address.py --cc <host-gcc> --output .local/cache-material-synthetic --material-fixture
python -m unittest discover -s tools/wii -p test_inspect_material_graph.py
```

For strict PPC compilation, use the existing compiler/SDK arguments with a new
ignored output directory. Compilation and authored host execution are separate
from PPC execution; this option does not stage SD files or launch Dolphin.
`--material-fixture` runs alone and rejects private/streaming/widget/BSP modes.

For a private material streaming build, add this option to the tag and BSP
streaming arguments documented above:

```text
--material-goldens <external-material-JSON>
```

This requires `--private-tag-data`, `--stream-file`, `--bsp-goldens`,
`--private-bsp-data` and `--bsp-stream-file`. It excludes `--widget-goldens`
and every standalone fixture option. The runner bounds both external raw inputs
to 22 MiB and both numeric metadata files to 1 MiB, compares their input hashes
and selected BSP identities, and rejects changed descriptor/path snapshots.
These checks are not a filesystem lock. Counts are generic aggregates, with
128 lightmaps and 2,048 materials per lightmap; 34 materials is not an API limit.
The selected byte count must equal 648 + 32L + 256M and fit the BSP input.
Material mode also reuses the BSP inspector on those snapshots, checking signed
descriptor counts, header/root/table bounds and disjointness, one-byte Data
spans, full scenario/BSP association and placement above the retained tag bytes.
Recomputed BSP goldens must match the supplied BSP metadata.

Generated material goldens contain unsigned numeric constants and full SHA-256
hex literals for inputs and selected serialization. The generated SHA unit
copies the existing `p2p_crypto.c` SHA-256 section unchanged; full source,
extracted section and generated-unit hashes are part of build identity. It
includes the SHA fixture header to check the extracted function declaration.
It does not import Linux platform, threading or external crypto dependencies.
Neither raw input is embedded in this streaming mode.

The material integration uses bounded direct offset streaming into the existing
22 MiB tag reservation, with the full original state/sound/index/IO capacities
and provisional reserve retained. Conservative widget workspace and serialization
slots remain charged even though this mode does not run a widget graph.
SD staging and input identity checks are separate private setup; this runner
does not create or update SD files. A successful PPC build establishes compilation,
not target execution, physical placement or Wii acceptance. Runtime inspection
compares selected serialization count, CRC and full SHA-256 with the numeric
goldens; it retains encoded addresses and makes no geometry execution claim.

The numeric inspector reads external raw tags and one declared BSP sidecar:

```powershell
python tools/wii/inspect_material_graph.py <external-tags> <external-BSP> --declared-map-length <inflated-map-bytes> --bsp-ordinal <ordinal> --output <new-external-JSON>
```

Every selected lightmap/material is validated, retaining source limits of
128 lightmaps, 2,048 materials per lightmap and 64,000 vertices per material.
Output is compact numeric summaries and hashes, bounded to 1 MiB; no tag names,
asset bytes or private paths are exported. Full salted shader identity, primary
group and shader ancestry are checked. NONE skips meaningless reference words;
name-length words remain opaque. Names must terminate within actual tags.
Material surface ranges use signed bounds, without a global ordering policy.
Non-null hardware words identify proper-kind 12-byte descriptors; count-derived
payload extents use type strides 56/32/20/8. NULL does not establish a payload
extent. Empty counts preserve words without following payloads. Xbox compressed
tag-data minimum is 32N + 8M, distinct from those hardware type strides.

The allocation-free C API measures actual aggregate workspace requirements,
then publishes into one of two caller-owned halves. Failures retain the prior
publication. Views validate their copied BSP/arena parent before child storage
access, then validate the independent material generation. Unload requires an
accessible control; active unload advances generation and inactive unload is
idempotent. Controls, truthful buffers and sources require immutable single-caller use.

Partial projections retain signed numeric fields and float bit patterns; unknown
bytes stay opaque. Inspection serialization copies the whole 648-byte root,
complete lightmap table, then each material table in lightmap order, rewriting
known LE words and retaining encoded addresses. It excludes payloads and shader
bodies. It is not a standalone relocated cache or save, full typed BSP/geometry
conversion, GX registration, engine memory strategy, gameplay or Wii acceptance.

The root also exposes the checked surface count and slot-relative surface span.
`cache_material_get_surface` returns one six-byte record as three explicitly
decoded LE unsigned 16-bit indices. It checks the copied parent/BSP/material
lifetimes before source access and preserves caller output on rejection. This
qualifies numeric triangle-record interpretation only. Vertex-index origin,
per-material vertex bounds, vertex conversion and rendering remain separate.

The root projection grows from 24 to 36 bytes, increasing the two-half workspace
by 24 payload bytes. For two lightmaps and 34 materials, payload is 8,856 bytes;
an authored model using compiled PPC layouts charges 64 additional arena bytes
after alignment, leaving 1,781,968 bytes after the provisional reserve. This
models planner placement and does not establish target execution. The six-byte output is caller
storage and adds no arena slot. Existing reservations and source capacities
remain. Selected serialization still copies only root/lightmap/material records,
retaining original surface count/address words and excluding triangle payloads.
Authored host cases and strict PPC compilation are separate from owned-data or
PPC execution; earlier streaming evidence applies to its recorded source build.
