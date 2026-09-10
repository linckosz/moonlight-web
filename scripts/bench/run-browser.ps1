# ============================================================================
# The BROWSER half of a campaign: the same matrix run-campaign.ps1 wrote, but
# played through a real stream instead of --native-bench. One pass per entry of
# results\matrix.json, and for each one the negotiated truth, the client legs
# and the click-to-photon series.
#
#   .\run-browser.ps1 [-KioskRect 0,0,2560,1440] [-Tile "Display 1"]
#
# Until now this half was a printed list of six commands to type twelve times
# per host. Everything below was learned by typing them, and every step exists
# because skipping it produced a plausible wrong number:
#
#   - The CONTENT kiosk and the CLIENT kiosk are two Chromes with two profiles.
#     kiosk.ps1 kills the Chromes of the profile it is about to use, so sharing
#     one profile makes the client kill the content — and a campaign then
#     measures a motionless desktop that encodes beautifully.
#   - Streaming this machine to itself raises "Streaming your own PC?", and the
#     tile stays stuck in app-card--launching until `.self-stream-go` is
#     clicked. Nothing times out; the pass simply never starts.
#   - The native host's tiles are named after the DISPLAY ("Display 1"), not
#     "Desktop": clicking the first "Desktop" on the page launches somebody
#     else's host.
#   - The host pointer parks on the captured screen, inside the inert click
#     target, and away from the top band where the flag is drawn — a click that
#     lands anywhere else activates a window, the client page goes hidden, rAF
#     freezes and the probe never returns.
#   - Click-to-photon needs a PHYSICAL screen. On a virtual display the flag is
#     never painted while the content captures perfectly, so every sample times
#     out and reads like a broken pipeline. run-campaign.ps1 warns; this script
#     records the verdict per pass rather than failing.
# ============================================================================
param(
    [string] $ResultsDir = '',
    [string] $KioskRect = '',
    [string] $AppUrl = 'https://127.0.0.1:8443/',
    [string] $ApiUrl = 'http://127.0.0.1:8080',
    [string] $Tile = '',
    [string] $ClientRect = '',
    [int]    $DebugPort = 9333,
    [int]    $Clicks = 10,
    [int]    $SpacingMs = 1500,
    [switch] $NoProbe,
    # The kiosks are borderless, TOPMOST and have no close button: whoever is at
    # the machine cannot get rid of them by hand. So this script owns their
    # lifetime and takes them down on the way out, however it leaves — the end
    # of the matrix, a throw, or an operator saying stop. -KeepKiosks is for
    # inspecting a finished pass; the way out is then Ctrl+Alt+Shift+Q, or
    # kiosk-close.ps1.
    [switch] $KeepKiosks
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $ResultsDir) { $ResultsDir = Join-Path $PSScriptRoot 'results' }
Set-Location $PSScriptRoot

function Get-Prop {
    param($Object, [string] $Name, $Default = $null)
    if ($null -eq $Object) { return $Default }
    if ($Object.PSObject.Properties.Name -contains $Name) { return $Object.$Name }
    return $Default
}

# ── The matrix, as the encoder half wrote it ────────────────────────────────
$matrixPath = Join-Path $ResultsDir 'matrix.json'
if (-not (Test-Path $matrixPath)) {
    throw "no matrix at $matrixPath — run run-campaign.ps1 first, it writes the plan both halves share"
}
$matrix = Get-Content $matrixPath -Raw -Encoding UTF8 | ConvertFrom-Json
$passes = @($matrix.passes)
# matrix.display is the whole /api/native/status entry, not the index.
$displayInfo = $matrix.display
$display = [int]$displayInfo.id

# ── Where the kiosks go, and where the pointer parks ────────────────────────
if (-not $KioskRect) {
    $inv = Get-Content (Join-Path $ResultsDir 'inventory.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $d = $displayInfo
    $mons = @()
    foreach ($line in @(Get-Prop $inv 'monitors' @())) {
        if ($line -match '^(\S+)\s+(-?\d+),(-?\d+)\s+(\d+)x(\d+)') {
            $mons += [pscustomobject]@{ x = [int]$Matches[2]; y = [int]$Matches[3]
                                        w = [int]$Matches[4]; h = [int]$Matches[5] }
        }
    }
    $m = if ($display -lt $mons.Count -and $mons[$display].w -eq [int]$d.width) { $mons[$display] }
         else { $mons | Where-Object { $_.w -eq [int]$d.width -and $_.h -eq [int]$d.height } | Select-Object -First 1 }
    if (-not $m) { throw "cannot place the kiosk: no monitor matches display $display — pass -KioskRect" }
    $KioskRect = "$($m.x),$($m.y),$($m.w),$($m.h)"
}
$rect = $KioskRect -split ','
$kx = [int]$rect[0]; $ky = [int]$rect[1]; $kw = [int]$rect[2]; $kh = [int]$rect[3]

# ── Where the CLIENT kiosk goes: anywhere but the captured screen ───────────
# It used to be a hard-coded 0,0,2560,1440, which worked only as long as the
# captured display was somewhere else. The moment the campaign started taking
# the PRIMARY screen — which is at 0,0 — the client kiosk landed exactly on top
# of the content one, and the pass captured the client watching itself: an
# infinite mirror instead of the reference clip, encoding beautifully and
# measuring nothing. Pick the largest monitor that does not overlap the
# captured rectangle, and say so when there is none.
if (-not $ClientRect) {
    $allMons = @()
    foreach ($line in @(Get-Prop (Get-Content (Join-Path $ResultsDir 'inventory.json') -Raw -Encoding UTF8 | ConvertFrom-Json) 'monitors' @())) {
        if ($line -match '^(\S+)\s+(-?\d+),(-?\d+)\s+(\d+)x(\d+)') {
            $allMons += [pscustomobject]@{ device = $Matches[1]
                                           x = [int]$Matches[2]; y = [int]$Matches[3]
                                           w = [int]$Matches[4]; h = [int]$Matches[5] }
        }
    }
    $free = @($allMons | Where-Object {
        -not ($_.x -lt ($kx + $kw) -and ($_.x + $_.w) -gt $kx -and
              $_.y -lt ($ky + $kh) -and ($_.y + $_.h) -gt $ky)
    } | Sort-Object { $_.w * $_.h } -Descending)
    if ($free.Count -eq 0) {
        throw ("every monitor overlaps the captured screen ($KioskRect): the client kiosk " +
               "would cover the content and the pass would film itself. Pass -ClientRect, or " +
               "capture a display that is not the only screen.")
    }
    $c = $free[0]
    $ClientRect = "$($c.x),$($c.y),$($c.w),$($c.h)"
    Write-Host "client rect  : $ClientRect ($($c.device))"
}
$crect = $ClientRect -split ','
$cx = [int]$crect[0]; $cy = [int]$crect[1]; $cw = [int]$crect[2]; $ch = [int]$crect[3]

# The flag occupies 44%..56% x 0..5% of the captured screen (LatencyFlag.h).
# The pointer parks well below it, centred, and the inert target is drawn there:
# a click inside the flag would be measuring the overlay's own window.
$targetSize = [Math]::Max(200, [int]($kw * 0.12))
$parkX = $kx + [int]($kw * 0.5)
$parkY = $ky + [int]($kh * 0.72)
$targetX = $parkX - [int]($targetSize / 2)
$targetY = $parkY - [int]($targetSize / 2)

Write-Host "matrix       : $($passes.Count) passes, display $display"
Write-Host "kiosk rect   : $KioskRect"
Write-Host "pointer park : $parkX,$parkY (inert target $targetSize px)"

# ── The tile: the native host names its apps after the display ──────────────
if (-not $Tile) {
    try {
        $status = Invoke-RestMethod -Uri "$ApiUrl/api/native/status" -Method Get -TimeoutSec 10
        $d = $status.displays | Where-Object { $_.id -eq $display } | Select-Object -First 1
        $Tile = Get-Prop $d 'label' ''
    } catch { }
    if (-not $Tile) { throw "cannot name the tile for display $display — pass -Tile" }
    Write-Host "tile         : $Tile (from /api/native/status)"
}

# ── The two kiosks and the click target ─────────────────────────────────────
# Content first: the client kiosk raises itself last, so it keeps the focus.
$content = 'file:///' + ((Join-Path $PSScriptRoot 'content\cod.html') -replace '\\', '/')
$clip = Join-Path $env:USERPROFILE '.mw-bench\content\cod.webm'
if (Test-Path $clip) {
    $content += '?src=' + [uri]::EscapeDataString('file:///' + ($clip -replace '\\', '/'))
}
& powershell -NoProfile -File "$PSScriptRoot\kiosk.ps1" -Url $content -X $kx -Y $ky -W $kw -H $kh | Out-Host
& powershell -NoProfile -File "$PSScriptRoot\kiosk.ps1" -Url $AppUrl -X $cx -Y $cy -W $cw -H $ch `
    -DebugPort $DebugPort | Out-Host

# -like '*click-target.ps1*' matches the killing shell's own command line: exclude $PID.
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
    Where-Object { $_.CommandLine -like '*click-target.ps1*' -and $_.ProcessId -ne $PID } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Process powershell -WindowStyle Hidden -ArgumentList @(
    '-NoProfile', '-File', "$PSScriptRoot\click-target.ps1",
    '-X', $targetX, '-Y', $targetY, '-Size', $targetSize)
Start-Sleep -Seconds 2

# ── Helpers over cdp.py ─────────────────────────────────────────────────────
function Cdp {
    param([Parameter(ValueFromRemainingArguments = $true)] $Args)
    & python "$PSScriptRoot\cdp.py" --port $DebugPort @Args 2>&1 | Out-String
}

function Stop-Stream {
    # Named "Stop Streaming" in the stream view; absent when no stream runs.
    $btns = Cdp eval "JSON.stringify([...document.querySelectorAll('button')].filter(e=>e.offsetParent).map(e=>e.textContent.trim()))"
    if ($btns -match 'Stop Streaming') {
        Cdp exitfs | Out-Null
        Cdp launch 'Stop Streaming' | Out-Null
        Start-Sleep -Seconds 4
    }
}

# ── Wait for the app to actually have tiles ─────────────────────────────────
# The client kiosk starts a COLD Chrome on a single-page app: the shell loads,
# the service worker answers, the host list arrives, and only then does a tile
# exist. Two seconds was enough on a warm profile and not on a cold one, and the
# failure is a throw on the FIRST pass - "no element with text 'Display 1'" -
# which is indistinguishable from a host that is not paired at all.
$ready = $false
for ($i = 0; $i -lt 40; $i++) {
    $txt = Cdp eval "JSON.stringify(document.body ? document.body.innerText : '')"
    if ($txt -match [regex]::Escape($Tile)) { $ready = $true; break }
    Start-Sleep -Seconds 2
}
if (-not $ready) {
    throw "the app at $AppUrl never showed a tile named '$Tile' - is the host paired in THIS instance? (cdp.py --port $DebugPort tiles says what is on the page)"
}
Write-Host "app ready    : tile '$Tile' is on the page"

$out = @()
$jsonl = Join-Path $ResultsDir 'browser.jsonl'
if (Test-Path $jsonl) { Remove-Item $jsonl }

# The kiosks are on screen from here until the finally below, whatever
# happens in between. Before this try existed, an interrupted campaign left
# a borderless TOPMOST video on the machine with no way to close it.
try {
    foreach ($pass in $passes) {
        $skip = Get-Prop $pass 'skip' ''
        if ($skip) {
            Write-Host "[SKIP] $($pass.id) — $skip"
            $out += [pscustomobject]@{ id = $pass.id; factor = $pass.factor; skipped = $skip }
            continue
        }
        Write-Host ""
        Write-Host "=== $($pass.id) ($($pass.factor)) ==="

        Stop-Stream
        # Through a FILE, never as an inline argument: PowerShell strips the double
        # quotes before python sees them, cdp.py's Object.assign then throws, the
        # page reloads with the settings it already had, and the pass measures the
        # reference while claiming to measure a factor. Twelve passes came back as
        # 1080p HEVC that way — the log line "Per-request streaming settings" is
        # what gives it away, so it is checked below.
        $settingsPath = Join-Path $ResultsDir "settings-$($pass.id).json"
        ($pass.settings | ConvertTo-Json -Compress) | Set-Content -Path $settingsPath -Encoding UTF8
        $applied = Cdp settingsfile $settingsPath
        if ($applied -notmatch [regex]::Escape($pass.settings.video_codec)) {
            Write-Warning "  settings did not take: $($applied.Trim())"
        }
        Start-Sleep -Seconds 2

        # Twice if the first only hovered, then the self-stream confirmation.
        Cdp launch $Tile | Out-Null
        Start-Sleep -Seconds 3
        $state = Cdp eval "JSON.stringify({selfGo:!!document.querySelector('.self-stream-go'),canvas:!!document.querySelector('canvas')})"
        if ($state -match '"selfGo":true') {
            Cdp launch 'Stream anyway 🚀' | Out-Null
            Start-Sleep -Seconds 8
        } elseif ($state -notmatch '"canvas":true') {
            Cdp launch $Tile | Out-Null
            Start-Sleep -Seconds 6
            if ((Cdp eval "!!document.querySelector('.self-stream-go')") -match 'True|true') {
                Cdp launch 'Stream anyway 🚀' | Out-Null
                Start-Sleep -Seconds 8
            }
        }

        $row = [ordered]@{ id = $pass.id; factor = $pass.factor }
        if ((Cdp eval "!!document.querySelector('canvas')") -notmatch 'True|true') {
            Write-Warning "  the stream never started"
            $row.error = 'stream never started'
            $out += [pscustomobject]$row
            ($row | ConvertTo-Json -Compress) | Add-Content -Path $jsonl -Encoding UTF8
            continue
        }

        Cdp fullscreen | Out-Null
        Start-Sleep -Seconds 2

        # The NEGOTIATED settings, which are not always the ones asked for. Read off
        # the overlay rows rather than the request: a HEVC that became H.264 and a
        # 4:4:4 that fell back to 4:2:0 are exactly what the campaign looks for.
        $rows = Cdp eval "JSON.stringify(Object.fromEntries([...document.querySelectorAll('.stats-row')].map(r=>[r.querySelector('.stats-label')?.textContent.trim(),r.querySelector('.stats-value')?.textContent.trim()])))"
        $row.negotiated = $rows.Trim()
        Write-Host "  negotiated : $($row.negotiated)"

        $row.perf = (Cdp perf 20).Trim()

        if (-not $NoProbe) {
            $probe = & powershell -NoProfile -File "$PSScriptRoot\probe-run.ps1" `
                -Label $pass.id -Clicks $Clicks -SpacingMs $SpacingMs `
                -ParkX $parkX -ParkY $parkY -DebugPort $DebugPort -ResultsDir $ResultsDir 2>&1 | Out-String
            $row.probe = $probe.Trim()
            $line = @($probe -split "`n" | Where-Object { $_ -match '"label"' }) | Select-Object -First 1
            if ($line) { Write-Host "  probe      : $($line.Trim())" }
        }

        $out += [pscustomobject]$row
        ($row | ConvertTo-Json -Compress -Depth 6) | Add-Content -Path $jsonl -Encoding UTF8
    }

    Stop-Stream
}
finally {
    if ($KeepKiosks) {
        Write-Host ''
        Write-Host 'kiosks left open (-KeepKiosks): Ctrl+Alt+Shift+M to minimise, Ctrl+Alt+Shift+Q to close.'
    } else {
        & powershell -NoProfile -File "$PSScriptRoot\kiosk-close.ps1" | Out-Host
    }
}
Write-Host ""
Write-Host "browser half written to $jsonl"
Write-Host "then: python report.py  ->  bench-out\report.html"
