# Wii development helper

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
