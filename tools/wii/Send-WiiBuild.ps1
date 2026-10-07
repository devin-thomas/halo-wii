<#
.SYNOPSIS
Send an existing Wii DOL/ELF through wiiload to an explicitly selected LAN Wii.
.DESCRIPTION
No installation, persistent environment changes, discovery, firmware operations,
asset copying or gameplay verification. Homebrew Channel must already be ready.
Use -WhatIf first. Authored for Windows PowerShell 5.1+ / PowerShell 7; not
runtime-tested by the planning-pack author.
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [Parameter(Mandatory = $true)]
    [string] $ConsoleAddress,

    [Parameter(Mandatory = $true)]
    [string] $WiiloadPath,

    [Parameter(Mandatory = $true)]
    [string] $BinaryPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Require ordinary dotted decimal, not DNS, hexadecimal or abbreviated IPv4.
if ($ConsoleAddress -notmatch '^(\d{1,3}\.){3}\d{1,3}$') {
    throw 'ConsoleAddress must be an explicit dotted-decimal private IPv4 address.'
}
$octets = @($ConsoleAddress.Split('.') | ForEach-Object { [int] $_ })
if (@($octets | Where-Object { $_ -lt 0 -or $_ -gt 255 }).Count -ne 0) {
    throw 'ConsoleAddress contains an invalid IPv4 octet.'
}
$isPrivate = ($octets[0] -eq 10) -or `
    ($octets[0] -eq 172 -and $octets[1] -ge 16 -and $octets[1] -le 31) -or `
    ($octets[0] -eq 192 -and $octets[1] -eq 168)
if (-not $isPrivate) {
    throw 'Only RFC1918 private IPv4 targets are allowed. Verify your actual LAN Wii address.'
}
$targetAddress = $octets -join '.'

$sender = Get-Item -LiteralPath $WiiloadPath -ErrorAction Stop
$binary = Get-Item -LiteralPath $BinaryPath -ErrorAction Stop
if ($sender.PSIsContainer -or $binary.PSIsContainer) {
    throw 'WiiloadPath and BinaryPath must both be files, not directories.'
}
if ($sender.Extension -ine '.exe') {
    throw 'WiiloadPath must select the installed Windows wiiload .exe explicitly.'
}
if (@('.dol', '.elf') -notcontains $binary.Extension.ToLowerInvariant()) {
    throw 'BinaryPath must be an existing .dol or .elf file.'
}
if ($binary.Length -le 0) {
    throw 'The selected Wii executable is empty.'
}
$hash = (Get-FileHash -LiteralPath $binary.FullName -Algorithm SHA256).Hash
Write-Host "Binary: $($binary.FullName)"
Write-Host "SHA256: $hash"
Write-Host "Target: $targetAddress (confirm Homebrew Channel is ready)"

if ($PSCmdlet.ShouldProcess($targetAddress, "Send $($binary.Name) using wiiload")) {
    $previousWiiload = [Environment]::GetEnvironmentVariable('WIILOAD', 'Process')
    try {
        [Environment]::SetEnvironmentVariable('WIILOAD', "tcp:$targetAddress", 'Process')
        & $sender.FullName $binary.FullName
        $senderExit = $LASTEXITCODE
        if ($senderExit -ne 0) {
            throw "wiiload failed with exit code $senderExit. Confirm HBC/network readiness before retrying."
        }
        Write-Host 'Sender completed. Confirm the new build ID on the Wii; this is not a hardware test pass.'
        [pscustomobject]@{
            ConsoleAddress = $targetAddress
            Binary = $binary.FullName
            Sha256 = $hash
            SenderExitCode = $senderExit
            HardwareVerified = $false
        }
    }
    finally {
        [Environment]::SetEnvironmentVariable('WIILOAD', $previousWiiload, 'Process')
    }
}
