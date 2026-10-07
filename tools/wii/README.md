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
