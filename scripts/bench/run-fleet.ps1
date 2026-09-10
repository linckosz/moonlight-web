# ============================================================================
# The FLEET half of a campaign: one REMOTE MoonlightWeb host, driven from the
# Chrome on this machine.
#
#   .\run-fleet.ps1 -Label um790pro -AppUrl https://<addr>:49443/ -Pin 312761
#
# The star matrix (run-campaign.ps1 + run-browser.ps1) measures the host that
# runs the campaign: both kiosks, the click target and the flag are on THIS
# machine's screens. A remote host inverts that — the content to be filmed and
# the flag are over there, only the measuring browser is here — so this script
# does the half that can be done from here, and says plainly what it cannot:
#
#   - NO click-to-photon. The flag is painted on the remote screen; reading it
#     from here works only if that machine can paint an overlay at all, and a
#     GNOME Wayland session cannot, by design. The campaign's own rule is that
#     such a case is grey with its reason, never red.
#   - The CONTENT must already be moving on the remote screen, or every pass
#     measures a still desktop: 14 KB a frame at QP 10, which looks like a
#     wonderful result and means nothing.
#
# What it does measure is what the reduced fleet matrix asks for: 1080p60,
# 4:2:0, SDR, enhancer off then on, times every codec that host declares — and
# for each one, what was NEGOTIATED against what was asked, plus the client
# legs from [perf].
# ============================================================================
param(
    [Parameter(Mandatory = $true)] [string] $Label,
    [Parameter(Mandatory = $true)] [string] $AppUrl,
    [string]   $Pin = '',
    [string]   $MachineName = 'bench',
    [string[]] $Codecs = @('hevc', 'h264'),
    [string]   $Tile = 'Display 1',
    [string]   $ResultsDir = '',
    [string]   $ClientRect = '',
    [int]      $DebugPort = 9333,
    [int]      $SettleSec = 14,
    [switch]   $KeepKiosk
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $ResultsDir) { $ResultsDir = Join-Path $PSScriptRoot 'results-fleet' }
New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null
Set-Location $PSScriptRoot

function Cdp {
    param([Parameter(ValueFromRemainingArguments = $true)] $Args)
    & python "$PSScriptRoot\cdp.py" --port $DebugPort @Args 2>&1 | Out-String
}

# Getting back to the library, which is where every pass has to start. A pass
# that ends in the stream view - or a reload that lands slowly - otherwise
# fails the NEXT launch on "no element with text 'Display 1'", which reads
# like a host that went away rather than a page that is simply elsewhere.
function Wait-Library {
    param([int] $Tries = 30, [switch] $Renavigate)
    for ($w = 0; $w -lt $Tries; $w++) {
        if ((Cdp eval "JSON.stringify(document.body ? document.body.innerText : '')") -match [regex]::Escape($Tile)) { return $true }
        Start-Sleep -Seconds 2
    }
    if ($Renavigate) {
        Cdp nav $AppUrl | Out-Null
        Start-Sleep -Seconds 6
        return (Wait-Library -Tries 20)
    }
    return $false
}

# ── Where the measuring browser goes ────────────────────────────────────────
# Any screen will do — it is not being filmed — but not the one a local campaign
# films, so the two can share a machine.
if (-not $ClientRect) {
    $pick = ''
    foreach ($line in @(& powershell -NoProfile -File "$PSScriptRoot\monitors.ps1")) {
        if ($line -match '^(\S+)\s+(-?\d+),(-?\d+)\s+(\d+)x(\d+)(\s+primary)?') {
            if ((-not $Matches[6]) -and ([int]$Matches[4] -ge 1280)) {
                $pick = "$($Matches[2]),$($Matches[3]),$($Matches[4]),$($Matches[5])"
                break
            }
        }
    }
    if (-not $pick) { $pick = '0,0,1920,1080' }
    $ClientRect = $pick
}
$r = $ClientRect -split ','
Write-Host "client kiosk : $ClientRect"
Write-Host "host         : $AppUrl"
# `powershell -File` cannot pass an array: -Codecs hevc,h264 arrives as the
# single string 'hevc,h264', which is not a codec - the app then falls back to
# H.264 and the run looks like two passes that both chose H.264 on their own.
$Codecs = @($Codecs | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
Write-Host "codecs       : $($Codecs -join ', ')"

& powershell -NoProfile -File "$PSScriptRoot\kiosk.ps1" -Url $AppUrl `
    -X ([int]$r[0]) -Y ([int]$r[1]) -W ([int]$r[2]) -H ([int]$r[3]) -DebugPort $DebugPort | Out-Host

try {
    # ── The PIN, when the host asks for one ─────────────────────────────────
    # A machine this browser has never unlocked shows the login page instead of
    # the library, and every later step then fails on "no element with text
    # 'Display 1'" — which reads like an unpaired host rather than a locked one.
    Start-Sleep -Seconds 8
    if ((Cdp eval "JSON.stringify(!!document.getElementById('login-pin-input'))") -match 'true') {
        if (-not $Pin) {
            throw "$AppUrl asks for a PIN and none was given (-Pin). Mint one ON that machine, on its loopback: GET /api/admin/token, then POST /api/admin/pin/generate with that value in X-MW-Admin-Key."
        }
        $js = "(()=>{const set=(el,v)=>{const p=Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype,'value').set;p.call(el,v);el.dispatchEvent(new Event('input',{bubbles:true}));el.dispatchEvent(new Event('change',{bubbles:true}));};set(document.getElementById('login-machine-input'),'$MachineName');set(document.getElementById('login-pin-input'),'$Pin');const b=[...document.querySelectorAll('button')].find(e=>e.textContent.trim()==='Unlock');if(!b)return 'no Unlock button';b.click();return 'submitted';})()"
        Write-Host "unlock       : $((Cdp eval $js).Trim())"
        Start-Sleep -Seconds 8
    }

    # ── Wait for the library, as run-browser.ps1 does ───────────────────────
    $ready = $false
    for ($i = 0; $i -lt 40; $i++) {
        if ((Cdp eval "JSON.stringify(document.body ? document.body.innerText : '')") -match [regex]::Escape($Tile)) {
            $ready = $true; break
        }
        Start-Sleep -Seconds 2
    }
    if (-not $ready) {
        throw "no tile named '$Tile' at $AppUrl — cdp.py --port $DebugPort tiles lists what is on the page. The native host names its tiles after the DISPLAY, so the first 'Desktop' on the page belongs to somebody else."
    }
    Write-Host "app ready    : tile '$Tile' is on the page"

    $jsonl = Join-Path $ResultsDir "$Label.jsonl"
    if (Test-Path $jsonl) { Remove-Item $jsonl }

    foreach ($codec in $Codecs) {
        foreach ($enh in @('off', 'on')) {
            $id = "$codec-enhancer-$enh"
            Write-Host ''
            Write-Host "=== $Label / $id ==="
            # Whatever the previous pass left on screen, start from the library.
            if ((Cdp eval "JSON.stringify([...document.querySelectorAll('button')].filter(e=>e.offsetParent).map(e=>e.textContent.trim()))") -match 'Stop Streaming') {
                Cdp exitfs | Out-Null
                Cdp launch 'Stop Streaming' | Out-Null
                Start-Sleep -Seconds 5
            }
            $algo = $enh   # the same word the star matrix writes: 'on' or 'off'
            $settings = '{"video_codec":"' + $codec + '","stream_height":1080,"stream_fps":60,' +
                        '"video_enhancement":"' + $algo + '","chroma_444_enabled":false,' +
                        '"hdr_enabled":false,"mute_host_audio":true,"stream_aspect":"auto"}'
            # Through a FILE, never inline. PowerShell strips the double quotes of an
            # inline argument before python sees them, so {"video_codec":"hevc"} arrives
            # as {video_codec:hevc}: the patch throws, the page reloads with the settings
            # it already had, and the pass silently measures those instead. Four fleet
            # passes came back as H.264 with no enhancer that way, and the host was
            # blamed for a request it had never received.
            $sf = Join-Path $ResultsDir "settings-$Label-$id.json"
            [System.IO.File]::WriteAllText($sf, $settings)
            Cdp settingsfile $sf | Out-Null
            Start-Sleep -Seconds 4
            # settingsfile ends with a Page.reload, so the library has to come back
            # before anything can be clicked.
            if (-not (Wait-Library -Renavigate)) {
                throw "after the settings reload the library never came back at $AppUrl"
            }
            # And the proof it landed: ask the page what it holds rather than trust that
            # the write worked.
            $got = (Cdp eval "localStorage.getItem('mw-streaming-settings')").Trim()
            if ($got -notmatch [regex]::Escape('"video_codec":"' + $codec + '"')) {
                throw "the settings patch did not land on $AppUrl - the page holds $got"
            }
            Cdp launch $Tile | Out-Null
            Start-Sleep -Seconds 5
            if ((Cdp eval "JSON.stringify([...document.querySelectorAll('button')].filter(e=>e.offsetParent).map(e=>e.textContent.trim()))") -match 'Stream anyway') {
                Cdp launch 'Stream anyway' | Out-Null
                Start-Sleep -Seconds 4
            }
            Cdp fullscreen | Out-Null
            Start-Sleep -Seconds $SettleSec

            $row = [ordered]@{
                label      = $Label
                id         = $id
                asked      = "1080p60 $codec 4:2:0 SDR enhancer=$enh"
                negotiated = (Cdp stats).Trim()
            }
            Write-Host "  negotiated : $($row.negotiated)"
            $row.perf = (Cdp perf 15).Trim()
            ([pscustomobject]$row | ConvertTo-Json -Compress -Depth 6) | Add-Content -Path $jsonl -Encoding UTF8

            Cdp exitfs | Out-Null
            if ((Cdp eval "JSON.stringify([...document.querySelectorAll('button')].filter(e=>e.offsetParent).map(e=>e.textContent.trim()))") -match 'Stop Streaming') {
                Cdp launch 'Stop Streaming' | Out-Null
                Start-Sleep -Seconds 5
            }
        }
    }
    Write-Host ''
    Write-Host "fleet half written to $jsonl"
}
finally {
    if ($KeepKiosk) {
        Write-Host 'kiosk left open (-KeepKiosk): Ctrl+Alt+Shift+Q closes it.'
    } else {
        & powershell -NoProfile -File "$PSScriptRoot\kiosk-close.ps1" -Client | Out-Host
    }
}
