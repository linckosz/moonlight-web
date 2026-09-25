# ============================================================================
# Windows HDR on the ONE physical screen a campaign measures, for the ONE pass
# that needs it (hdr-on), and back the way it was, whatever happens.
#
#   . "$PSScriptRoot\hdr-switch.ps1"
#   $was = Enter-PassHdr '\\.\DISPLAY5'      # HDR on, returns the state it found
#   try { ...the pass... } finally { Exit-PassHdr '\\.\DISPLAY5' $was }
#
# Dot-sourced by run-matrix.ps1 (the encoder half) and run-browser.ps1 (the
# browser half), which each play the hdr-on pass at their own time.
#
# Physical, because click-to-photon needs a physical screen (the flag is never
# painted on a virtual one); by GDI name, because a desk of three identical
# monitors cannot be told apart by model name. Bruno allowed exactly this on
# 25/09/2026: HDR, never the mode, and always restored. A finally runs on
# Ctrl+C too; a process killed outright is the one case it cannot cover, and
# `scripts\Set-DisplayHdr.ps1 -List` says afterwards what was left.
# ============================================================================

$script:SetDisplayHdr = Join-Path (Split-Path $PSScriptRoot -Parent) 'Set-DisplayHdr.ps1'

function Get-PassHdr([string] $Device) {
    $line = & powershell -NoProfile -File $script:SetDisplayHdr -List |
        Where-Object { $_ -like "$Device *" } | Select-Object -First 1
    if (-not $line) { throw "no active display named $Device" }
    return [bool]($line -match 'enabled=True')
}

function Set-PassHdr([string] $Device, [bool] $On) {
    $switch = if ($On) { '-Enable' } else { '-Disable' }
    & powershell -NoProfile -File $script:SetDisplayHdr -Device $Device $switch | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "could not turn HDR $(if ($On) { 'on' } else { 'off' }) on $Device" }
}

# HDR on for the pass; returns what it was, for Exit-PassHdr.
function Enter-PassHdr([string] $Device) {
    $was = Get-PassHdr $Device
    if (-not $was) {
        Set-PassHdr $Device $true
        # The compositor and the capture settle into the new format.
        Start-Sleep -Seconds 3
    }
    return $was
}

function Exit-PassHdr([string] $Device, [bool] $Was) {
    try {
        if ((Get-PassHdr $Device) -ne $Was) { Set-PassHdr $Device $Was }
    } catch {
        Write-Warning "HDR on $Device could not be put back to $(if ($Was) { 'on' } else { 'off' }): $_ - check scripts\Set-DisplayHdr.ps1 -List"
    }
}
