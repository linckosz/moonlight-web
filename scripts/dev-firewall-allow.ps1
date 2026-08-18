<#
.SYNOPSIS
    Pre-authorise MoonlightWeb dev/test builds in Windows Defender Firewall so no
    "Windows Security Alert" popup appears each time a fresh build (or the isolated
    --dev scratchpad instance) starts listening.

.DESCRIPTION
    NOTE: this machine runs Windows Defender Firewall, not Bitdefender. Defender
    application rules are keyed on the *exact* exe path (no wildcards), so allowing
    a build by path is useless the next time: the --dev instance is copied into a
    per-session scratchpad folder whose path carries a fresh UUID each run, so a new
    path never matches an old rule and Windows prompts again.

    The durable fix is to allow by *port* (path-independent). This script creates
    persistent inbound allow rules, on all profiles, for the fixed ports the dev
    instance binds:

        TCP 48080 / 48443   dev HTTP / HTTPS  (kDevHttpPort / kDevHttpsPort)
        TCP+UDP 48010-48014  WebRTC media range in internet/UPnP mode (kUpnpPort +4)

    It is idempotent (removes its own rules first) and self-elevates (one UAC prompt).
    It also cleans up dead auto-created rules that point at a now-deleted scratchpad
    UUID path, to keep the firewall list tidy.

    Local LAN streaming uses *dynamic* UDP ports which cannot be pinned by number; a
    full-stream test may still prompt once for UDP. The recurring startup popup
    (HTTP/HTTPS listener) is fully suppressed by the TCP rules above.

.NOTES
    Run once, elevated:  powershell -ExecutionPolicy Bypass -File scripts\dev-firewall-allow.ps1
#>

[CmdletBinding()]
param(
    [switch]$CleanupStale = $true
)

# --- self-elevate ---------------------------------------------------------
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Not elevated - relaunching with UAC..." -ForegroundColor Yellow
    $psi = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($CleanupStale) { $psi += '-CleanupStale' }
    Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $psi
    return
}

$Group = 'MoonlightWeb Dev'

# --- remove our previous rules (idempotent) -------------------------------
Get-NetFirewallRule -Group $Group -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction SilentlyContinue

# --- create port-based allow rules ----------------------------------------
$common = @{
    Group     = $Group
    Direction = 'Inbound'
    Action    = 'Allow'
    Profile   = 'Any'
    Enabled   = 'True'
}

New-NetFirewallRule @common -DisplayName 'MoonlightWeb dev HTTP/HTTPS (TCP 48080/48443)' `
    -Protocol TCP -LocalPort 48080, 48443 | Out-Null

New-NetFirewallRule @common -DisplayName 'MoonlightWeb WebRTC media UPnP (UDP 48010-48014)' `
    -Protocol UDP -LocalPort '48010-48014' | Out-Null

New-NetFirewallRule @common -DisplayName 'MoonlightWeb WebRTC ICE-TCP UPnP (TCP 48010-48014)' `
    -Protocol TCP -LocalPort '48010-48014' | Out-Null

Write-Host "Created port-based allow rules in group '$Group'." -ForegroundColor Green

# --- cleanup dead scratchpad program rules --------------------------------
if ($CleanupStale) {
    $removed = 0
    Get-NetFirewallApplicationFilter -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Program -and
            $_.Program -match 'moonlightweb\.exe$' -and
            $_.Program -match '\\temp\\claude\\' -and
            -not (Test-Path -LiteralPath $_.Program)
        } |
        ForEach-Object {
            $_ | Get-NetFirewallRule -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction SilentlyContinue
            $removed++
        }
    Write-Host "Removed $removed dead scratchpad rule(s)." -ForegroundColor Green
}

Write-Host ""
Get-NetFirewallRule -Group $Group | Select-Object DisplayName, Enabled, Direction, Action | Format-Table -Auto
