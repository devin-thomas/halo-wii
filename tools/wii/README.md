# Wii development helper

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
