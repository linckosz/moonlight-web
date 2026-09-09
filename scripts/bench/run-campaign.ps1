# ============================================================================
# MoonlightWeb — the bench campaign driver.
#
#   .\run-campaign.ps1                      # discover, plan, run, report
#   .\run-campaign.ps1 -PlanOnly            # show the matrix that WOULD be run
#   .\run-campaign.ps1 -EncoderOnly         # the --native-bench half only
#   .\run-campaign.ps1 -Display 1 -Content cod
#
# ── The experiment design: a star, plus a sweep ─────────────────────────────
#
# The full product of codec x enhancer x 4:4:4 x HDR x mute x six modes across
# a fleet is four hundred passes and several days. The star design gets the same
# answers in twelve: fix a reference point, and move exactly ONE factor at a
# time around it. Every difference is then attributable to the factor that
# moved, which the full product does not give you either — it gives you
# interactions nobody has time to read.
#
# Reference: 1080p60, HEVC, enhancer off, 4:2:0, SDR, host muted, bitrate and
# aspect on automatic, the looped Call of Duty clip as content.
#
# The reference is replayed at the HEAD and the TAIL of every matrix. If the two
# disagree by more than 15% the machine was not in a steady state and the whole
# matrix is worthless — better to know that than to publish it.
#
# ── What is skipped, and what is merely observed ────────────────────────────
#
# A codec the host cannot encode is skipped, with the reason recorded: the
# inventory knows it, and running it would only measure the fallback. Everything
# else is RUN and then compared against what was negotiated — 4:4:4 silently
# becoming 4:2:0, HDR silently becoming SDR and HEVC silently becoming H.264 are
# exactly the findings a campaign exists to catch, and pre-judging them from a
# capability table would hide all three.
# ============================================================================
param(
    [string] $ResultsDir = "$PSScriptRoot\results",
    [switch] $PlanOnly,
    [switch] $EncoderOnly,
    # Leave whatever is on the captured display alone. For a dry run of the
    # machinery, and for a machine somebody is sitting in front of — the kiosk
    # takes a whole screen. The numbers are then about that desktop, not about
    # the reference clip, so they are not comparable with a real campaign.
    [switch] $NoKiosk,
    [switch] $SkipDiscover,
    # -1 picks the PRIMARY screen, which is the physical one on every bench
    # here. Click-to-photon needs that: see the monitor pairing below.
    [int]    $Display = -1,
    [ValidateSet('cod', 'scroll', 'still')] [string] $Content = 'cod',
    [string] $KioskRect = '',
    [string] $Exe = "$PSScriptRoot\..\..\build\MoonlightWeb.exe"
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null

# ── 1. What is here today ───────────────────────────────────────────────────

$inventoryPath = Join-Path $ResultsDir 'inventory.json'
if (-not $SkipDiscover -or -not (Test-Path $inventoryPath)) {
    & powershell -NoProfile -File "$PSScriptRoot\discover.ps1" -ResultsDir $ResultsDir
}
$inventory = Get-Content $inventoryPath -Raw -Encoding UTF8 | ConvertFrom-Json

$local = $inventory.machines | Where-Object { $_.kind -eq 'local' } | Select-Object -First 1
if (-not $local -or -not $local.native) {
    throw "no MoonlightWeb answering on loopback — start one (--dev) before a campaign"
}

# /api/native/status omits a property rather than sending it null: encoder_name,
# encoder_hardware and encoder_is_fallback exist ONLY when the engine fell back
# to a software encoder. Under StrictMode reading an absent property throws, so
# every optional field goes through here.
function Get-Prop {
    param($Object, [string] $Name, $Default = $null)
    if ($null -eq $Object) { return $Default }
    if ($Object.PSObject.Properties.Name -contains $Name) { return $Object.$Name }
    return $Default
}

# The monitors as Windows enumerates them, parsed once: the origin of the kiosk
# rectangle comes from here, and so does the primary/virtual verdict below.
# /api/native/status carries neither.
$monitors = @()
foreach ($line in @(Get-Prop $inventory 'monitors' @())) {
    if ($line -match '^(\S+)\s+(-?\d+),(-?\d+)\s+(\d+)x(\d+)(\s+primary)?') {
        $monitors += [pscustomobject]@{
            device = $Matches[1]; x = [int]$Matches[2]; y = [int]$Matches[3]
            w = [int]$Matches[4]; h = [int]$Matches[5]; primary = [bool]$Matches[6]
        }
    }
}

# Which display to measure. -Display -1 means "the primary one", and that is the
# default because click-to-photon does not work anywhere else here: the flag is
# a layered topmost window, and on a VIRTUAL display (a VDD / dummy plug) it is
# never painted, while the content Chrome puts on the same screen captures
# perfectly. The result is a campaign whose encoder half looks flawless and
# whose every click comes back a timeout — measured 09/09/2026, on a physical
# screen the three bands read 0,0,255 / 255,255,255 / 255,0,0 exactly.
if ($Display -lt 0) {
    $primaryIdx = -1
    for ($i = 0; $i -lt $monitors.Count; $i++) { if ($monitors[$i].primary) { $primaryIdx = $i; break } }
    $Display = if ($primaryIdx -ge 0 -and
                   ($local.native.displays | Where-Object { $_.id -eq $primaryIdx })) { $primaryIdx } else { 0 }
    Write-Host "display            : $Display (primary — pass -Display to override)"
}

# The display the campaign measures, and what it can do.
$displayInfo = $local.native.displays | Where-Object { $_.id -eq $Display } | Select-Object -First 1
if (-not $displayInfo) {
    Write-Warning "no display with id $Display; available:"
    $local.native.displays | Format-Table id, label, width, height, refresh_mhz, hdr_active, encoder_name -AutoSize
    throw "pick one with -Display"
}

# A virtual screen cannot carry the flag (see above), and the campaign should
# say so BEFORE it spends twenty minutes rather than after. The test is the
# monitor description the engine reports, which is the EDID name: a dummy plug
# or a driver-made screen announces itself there ("VDD by MTT", "IddSampleDriver"
# and friends). It is a heuristic on a string, so it only warns — the reliable
# signal is the one the probe itself gives, three grey pixels where the bands
# should be.
$displayDetail = "$(Get-Prop $displayInfo 'detail' '') $(Get-Prop $displayInfo 'label' '')"
$looksVirtual = $displayDetail -match '(?i)\b(vdd|virtual|idd|dummy|phantom)\b'
if ($looksVirtual) {
    Write-Warning ("display $Display looks like a VIRTUAL screen ($($displayDetail.Trim())). " +
                   "The encoder half is valid there, but the click-to-photon flag is never " +
                   "painted on such a screen: every sample will time out. Measure a physical " +
                   "one, or read the probe's cases as grey and not as a failure.")
}
# /api/native/status sends `codecs` as one comma-separated string ("AV1, HEVC,
# H.264"), not as an array — treating it as a list gives a single element that
# matches nothing, and the campaign then skips every codec while reporting that
# the host supports them all.
$hostCodecsRaw = Get-Prop $displayInfo 'codecs' ''
$hostCodecs = @($hostCodecsRaw -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$encoder = Get-Prop $displayInfo 'encoder' '?'
if (Get-Prop $displayInfo 'encoder_is_fallback' $false) {
    # Worth shouting: a fallback encoder means no GPU took the job, and every
    # number from this machine has to be read as a software figure.
    $encoder = "$encoder (FALLBACK: $(Get-Prop $displayInfo 'encoder_name' '?'))"
}
Write-Host ""
Write-Host "host display $Display : $($displayInfo.width)x$($displayInfo.height) $encoder codecs=$($hostCodecs -join ',') hdr_active=$(Get-Prop $displayInfo 'hdr_active' $false)"
Write-Host "click-to-photon    : supported=$(Get-Prop $local.native 'latencyFlagSupported' $false) $(Get-Prop $local.native 'latencyFlagReason' '')"

# The kiosk must cover the display being captured, and that display just told us
# how big it is — no reason to carry a screen size as a default and be wrong on
# every other bench. -KioskRect still overrides, for a window that must sit
# somewhere else.
if (-not $KioskRect) {
    # The SIZE comes from the display we are about to capture; the ORIGIN has to
    # come from Windows. A campaign that keeps 0,0 puts the kiosk on the primary
    # screen while capturing another one, and then measures a motionless desktop
    # — 14 KB a frame, QP 10, and numbers that look wonderful.
    #
    # /api/native/status carries no origin, so the two lists are paired here:
    # inventory.monitors is EnumDisplayMonitors' order, native displays are the
    # engine's, and both have been observed to agree. Trust that only when the
    # sizes match; otherwise fall back to the first monitor of the right size,
    # and say so rather than silently measuring the wrong screen.
    $w = [int]$displayInfo.width; $h = [int]$displayInfo.height
    $match = $null
    if ($Display -ge 0 -and $Display -lt $monitors.Count -and
        $monitors[$Display].w -eq $w -and $monitors[$Display].h -eq $h) {
        $match = $monitors[$Display]
    } else {
        $match = $monitors | Where-Object { $_.w -eq $w -and $_.h -eq $h } | Select-Object -First 1
        if ($match) {
            Write-Warning ("display $Display did not line up with monitor $Display; " +
                           "using $($match.device) at $($match.x),$($match.y) on its size alone")
        }
    }
    if (-not $match) {
        Write-Warning ("no monitor measures ${w}x${h}: falling back to 0,0, which is the " +
                       "primary screen. VERIFY the kiosk covers the captured display, or " +
                       "pass -KioskRect — a wrong one measures a motionless desktop.")
        $match = [pscustomobject]@{ device = '(unknown)'; x = 0; y = 0; w = $w; h = $h }
    }
    $KioskRect = "$($match.x),$($match.y),$w,$h"
    Write-Host "kiosk rect         : $KioskRect (display $Display = $($match.device))"
}

# ── 2. The matrix ───────────────────────────────────────────────────────────

$reference = [ordered]@{
    video_codec           = 'hevc'
    stream_height         = 1080
    stream_fps            = 60
    video_enhancement     = 'off'
    chroma_444_enabled    = $false
    hdr_enabled           = $false
    mute_host_audio       = $true
    stream_aspect         = 'auto'
}

function New-Pass {
    param([string] $Id, [string] $Factor, [hashtable] $Override = @{}, [string] $Skip = '')
    $settings = [ordered]@{}
    foreach ($k in $reference.Keys) { $settings[$k] = $reference[$k] }
    foreach ($k in $Override.Keys) { $settings[$k] = $Override[$k] }
    [pscustomobject]@{
        id       = $Id
        factor   = $Factor
        settings = $settings
        skip     = $Skip
    }
}

$passes = @()
$passes += New-Pass 'ref-head' 'reference'

# — the star: one factor at a time —
# The API spells them "H.264", "HEVC", "AV1" and the settings spell them
# "h264", "hevc", "av1": compare on a normalised form, or every codec looks
# unsupported and the campaign silently shrinks to nothing.
$hostCodecsNorm = @($hostCodecs | ForEach-Object { ($_ -replace '[^A-Za-z0-9]', '').ToLower() })
foreach ($codec in @('h264', 'av1')) {
    $skip = if ($hostCodecsNorm -contains $codec) { '' }
            else { "the host cannot encode $codec on this display (codecs: $($hostCodecs -join ', '))" }
    $passes += New-Pass "codec-$codec" 'codec' @{ video_codec = $codec } $skip
}
$passes += New-Pass 'enhancer-on' 'enhancer' @{ video_enhancement = 'on' }
$passes += New-Pass 'chroma-444' 'chroma' @{ chroma_444_enabled = $true }
$hdrSkip = if (Get-Prop $displayInfo 'hdr_active' $false) { '' }
           else { 'HDR is not switched on for this display in the OS — turn it on (scripts\Set-DisplayHdr.ps1) or this measures SDR' }
$passes += New-Pass 'hdr-on' 'hdr' @{ hdr_enabled = $true } $hdrSkip
$passes += New-Pass 'mute-off' 'mute' @{ mute_host_audio = $false }

# — the sweep: resolution and cadence, everything else at the reference —
#
# Deliberately NOT gated on the display's declared refresh rate. On this bench
# /api/native/status reports 60 Hz for a display Windows drives at 164 Hz, and
# the capture then delivers 135 frames a second — so a skip rule built on that
# field would have silently thrown away every 120 fps pass while reporting the
# host as incapable. Every mode is run, and the report compares the cadence
# ASKED FOR against the capture rate actually achieved: a shortfall is a finding
# with a number attached, which a skip never is.
foreach ($mode in @(@(720, 60), @(720, 120), @(1080, 120), @(1440, 60), @(1440, 120))) {
    $h = $mode[0]; $f = $mode[1]
    $passes += New-Pass "sweep-${h}p$f" 'resolution/fps' @{ stream_height = $h; stream_fps = $f }
}

$passes += New-Pass 'ref-tail' 'reference'

$matrix = [ordered]@{
    generatedAt = (Get-Date).ToString('o')
    commit      = $inventory.commit
    display     = $displayInfo
    reference   = $reference
    passes      = $passes
}
$matrixPath = Join-Path $ResultsDir 'matrix.json'
$matrix | ConvertTo-Json -Depth 8 | Set-Content -Path $matrixPath -Encoding UTF8

# ── Provenance: WHICH binary produced these numbers ─────────────────────────
#
# A campaign of record runs on the CI artifact, installed the way a user
# installs it; a diagnosis run uses the local build. Both are legitimate and
# they are not comparable, so the report has to say which one it was — mixing
# the two in silence is exactly what makes two campaigns incomparable.
#
# The hash, not the version string: the displayed version comes from a CMake
# cache and has been seen surviving a rebuild, so it does not prove which code
# is running. A digest of the file does.
$exeItem = Get-Item $Exe -ErrorAction SilentlyContinue
$provenance = [ordered]@{
    exe        = $Exe
    tier       = if ($Exe -like '*\build\*' -or $Exe -like '*/build/*') { 'local-build' }
                 elseif ($Exe -like '*Program Files*') { 'installed' }
                 else { 'artifact' }
    sha256     = if ($exeItem) { (Get-FileHash -Path $Exe -Algorithm SHA256).Hash.Substring(0, 16) } else { $null }
    fileVersion = if ($exeItem) { $exeItem.VersionInfo.FileVersion } else { $null }
    builtAt    = if ($exeItem) { $exeItem.LastWriteTime.ToString('o') } else { $null }
    commit     = $inventory.commit
    clipSha256 = $null
}
$clipForHash = Join-Path $env:USERPROFILE '.mw-bench\content\cod.webm'
if (Test-Path $clipForHash) {
    $provenance.clipSha256 = (Get-FileHash -Path $clipForHash -Algorithm SHA256).Hash.Substring(0, 16)
}
$provenance | ConvertTo-Json | Set-Content -Path (Join-Path $ResultsDir 'provenance.json') -Encoding UTF8
Write-Host ""
Write-Host "binary: $($provenance.tier) · $($provenance.fileVersion) · sha $($provenance.sha256)"
if ($provenance.tier -eq 'local-build') {
    Write-Host "        (a campaign of record runs on the CI artifact — see docs/bench-campaign.md §11)"
}

Write-Host ""
Write-Host "matrix: $($passes.Count) passes ($(@($passes | Where-Object { $_.skip }).Count) skipped)"
$passes | ForEach-Object {
    $flag = if ($_.skip) { 'SKIP' } else { ' run' }
    "  [$flag] $($_.id.PadRight(16)) $($_.factor)$(if ($_.skip) { "  — $($_.skip)" })"
}

if ($PlanOnly) {
    Write-Host ""
    Write-Host "plan only — nothing was run. Matrix written to $matrixPath"
    return
}

# ── 3. Content on the captured display ──────────────────────────────────────

$contentUrl = $null
if ($Content -eq 'cod') {
    $clip = Join-Path $env:USERPROFILE '.mw-bench\content\cod.webm'
    if (Test-Path $clip) {
        $src = 'file:///' + ($clip -replace '\\', '/')
        $page = 'file:///' + (((Join-Path $PSScriptRoot 'content\cod.html')) -replace '\\', '/')
        $contentUrl = "$page`?src=$([uri]::EscapeDataString($src))"
    } else {
        Write-Warning "no clip in the cache — run fetch-content.ps1; falling back to the scrolling text"
        $Content = 'scroll'
    }
}
if (-not $contentUrl) {
    $contentUrl = 'file:///' + (((Join-Path $PSScriptRoot "content\$Content.html")) -replace '\\', '/')
}
Write-Host ""
Write-Host "content: $contentUrl"

# ── 4. The encoder half ─────────────────────────────────────────────────────

$specs = @()
$specLabels = @()
foreach ($p in $passes) {
    if ($p.skip) { continue }
    $s = $p.settings
    $w = [int][math]::Round($s.stream_height * 16.0 / 9.0 / 2) * 2
    $spec = "codec=$($s.video_codec),fps=$($s.stream_fps),width=$w,height=$($s.stream_height)"
    if ($s.chroma_444_enabled) { $spec += ',yuv444=1' }
    if ($s.hdr_enabled) { $spec += ',hdr=1' }
    $specs += $spec
    $specLabels += $p.id
}

Write-Host ""
Write-Host "=== encoder half: $($specs.Count) --native-bench passes ==="
# Through a file, never as an array argument: a spec contains commas and
# `powershell -File` flattens an array into positional arguments, which turns
# the second spec into the value of the next parameter.
$specFile = Join-Path $ResultsDir 'specs.txt'
Set-Content -Path $specFile -Value $specs -Encoding UTF8
$matrixArgs = @('-NoProfile', '-File', "$PSScriptRoot\run-matrix.ps1",
                '-Display', $Display, '-SpecFile', $specFile,
                '-ResultsDir', $ResultsDir, '-Exe', $Exe)
if (-not $NoKiosk) {
    $matrixArgs += @('-Relaunch', '-ContentUrl', $contentUrl, '-KioskRect', $KioskRect)
} else {
    Write-Warning "-NoKiosk: measuring whatever is on display $Display, not the reference clip — these numbers are not comparable with a real campaign"
}
& powershell @matrixArgs

if ($EncoderOnly) {
    Write-Host ""
    Write-Host "encoder half done. The browser half (click-to-photon and the client legs) needs the"
    Write-Host "client kiosk — see docs/bench-campaign.md, or run without -EncoderOnly."
    return
}

# ── 5. The browser half ─────────────────────────────────────────────────────
#
# Deliberately NOT automated end to end here. A stream pass needs a paired host
# in the dev instance, a PIN, a tile clicked by coordinates (twice, the first
# click often only hovers), a page reload between two streams, and a host
# pointer parked on an inert target — every one of them a step that fails in a
# way a script cannot tell from a real regression. The skill drives it with the
# playbook at hand; these scripts give it the pieces.
Write-Host ""
Write-Host "=== browser half ==="
Write-Host "Pieces, in the order the protocol uses them:"
Write-Host "  1. kiosk.ps1 -Url <content>  -X.. -Y.. -W.. -H..        (content on the captured display)"
Write-Host "  2. kiosk.ps1 -Url <app url>  -DebugPort 9333 ...        (the measuring client)"
Write-Host "  3. click-target.ps1 -X .. -Y ..                         (where the host pointer parks)"
Write-Host "  4. cdp.py settings '<json>' ; cdp.py launch '<tile>' ; cdp.py fullscreen"
Write-Host "  5. cdp.py stats  /  cdp.py perf 20                      (negotiated truth + legs)"
Write-Host "  6. probe-run.ps1 -Label <pass id>                       (click-to-photon)"
Write-Host ""
Write-Host "then: python report.py  ->  bench-out\report.html"
