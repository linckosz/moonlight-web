# ============================================================================
# The FLEET half of a campaign: one REMOTE MoonlightWeb host, driven from the
# Chrome on this machine.
#
#   python fleet-run.py --machine <id> -- -Label um790pro [-AppUrl https://<addr>:49443/]
#
# fleet-run.py is the way in: it mints the machine's PIN in its own memory
# (acceptance/fleet.py, mint_pin) and starts this script with the PIN written
# to its standard input. That is the only way the PIN comes: no parameter, no
# prompt, and no command that prints it for a pipe — the script refuses to
# start when its input is a console, so nobody types it into a terminal, and
# no session of an assistant ever sees it, copies it or puts it in a command,
# nor captures it from an output. It is never shown,
# typed into the page by cdp.py insert-stdin, and dropped at the end. The
# client kiosk has a throwaway profile (.chrome-fleet): at the end, whatever
# happened, the page logs out (the host revokes the session), the kiosk closes
# and the profile goes. The run's own output and results are then searched for
# a PIN, by its shape: the count only, 0 expected.
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
if ($KeepKiosk) {
    throw '-KeepKiosk: a kiosk that holds a session on the host is never left open'
}
# The PIN: one line of standard input, from the process that minted it.
if (-not [Console]::IsInputRedirected) {
    throw 'the PIN comes on standard input, piped from the command that mints it - never typed: see the header'
}
$Pin = [Console]::In.ReadLine()
# A byte order mark, when the minting side pipes in UTF-8, is no part of it.
if ($Pin) { $Pin = $Pin.TrimStart([char]0xFEFF) }
if ($Pin -notmatch '^\d{4,8}$') {
    $Pin = $null
    throw 'no PIN on standard input (one line of 4 to 8 digits)'
}
if (-not $ResultsDir) { $ResultsDir = Join-Path $PSScriptRoot 'results-fleet' }
New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null
Set-Location $PSScriptRoot
$profileName = '.chrome-fleet'
$fleetWarnings = @()

# What this run says, kept as well for the final count of PINs in it.
$said = New-Object System.Collections.Generic.List[string]
function Say([string] $Text = '') {
    $script:said.Add($Text)
    Write-Host $Text
}

function Cdp {
    param([Parameter(ValueFromRemainingArguments = $true)] $Args)
    & python "$PSScriptRoot\cdp.py" --port $DebugPort @Args 2>&1 | Out-String
}

# Getting back to the library, which is where every pass has to start. A pass
# that ends in the stream view - or a reload that lands slowly - otherwise
# fails the NEXT launch on "no element with text 'Display 1'", which reads
# like a host that went away rather than a page that is simply elsewhere.
# Unlocking, as its own step because it happens more than once: a reload can
# drop the session, and the page that comes back is the login page, not the
# library. Ticking "keep me signed in" is what makes the session survive one -
# without it the matrix dies on the first settings reload, and the error says
# the library never came back, which sounds like the host went away.
function Invoke-Unlock {
    if (-not $script:Pin) { return $false }
    if ((Cdp eval "JSON.stringify(!!document.getElementById('login-pin-input'))") -notmatch 'true') { return $false }
    # The machine's name and the remember box first (a click on the box takes
    # the focus), then the PIN field emptied and focused: no value of the PIN
    # in any script. The answer is a boolean.
    $js = "(()=>{const set=(el,v)=>{const p=Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype,'value').set;p.call(el,v);el.dispatchEvent(new Event('input',{bubbles:true}));el.dispatchEvent(new Event('change',{bubbles:true}));};set(document.getElementById('login-machine-input'),'$MachineName');const k=document.getElementById('login-remember');if(k&&!k.checked){k.click();}const i=document.getElementById('login-pin-input');set(i,'');i.focus();return document.activeElement===i;})()"
    if ((Cdp eval $js) -notmatch 'true') {
        Say 'unlock       : no PIN field to type into'
        return $true
    }
    # The PIN through a pipe to cdp.py, which types it and answers ok or failed;
    # neither its output nor an error is shown beyond that word.
    $typed = 'failed'
    try {
        $typed = ($script:Pin | & python "$PSScriptRoot\cdp.py" --port $DebugPort insert-stdin 2>$null | Out-String).Trim()
    } catch { $typed = 'failed' }
    if ($typed -ne 'ok') {
        Say 'unlock       : the PIN could not be typed'
        return $true
    }
    $sent = Cdp eval "(()=>{const b=[...document.querySelectorAll('button')].find(e=>e.textContent.trim()==='Unlock');if(!b)return false;b.click();return true;})()"
    Say "unlock       : submitted $($sent -match 'true')"
    Start-Sleep -Seconds 8
    return $true
}

function Wait-Library {
    param([int] $Tries = 30, [switch] $Renavigate)
    for ($w = 0; $w -lt $Tries; $w++) {
        if (Invoke-Unlock) { continue }
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
Say "client kiosk : $ClientRect"
Say "host         : $AppUrl"
# `powershell -File` cannot pass an array: -Codecs hevc,h264 arrives as the
# single string 'hevc,h264', which is not a codec - the app then falls back to
# H.264 and the run looks like two passes that both chose H.264 on their own.
$Codecs = @($Codecs | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
Say "codecs       : $($Codecs -join ', ')"

try {
    # Inside the try: from the moment the kiosk exists, the finally ends it.
    & powershell -NoProfile -File "$PSScriptRoot\kiosk.ps1" -Url $AppUrl `
        -X ([int]$r[0]) -Y ([int]$r[1]) -W ([int]$r[2]) -H ([int]$r[3]) -DebugPort $DebugPort `
        -ChromeProfile $profileName | ForEach-Object { Say "$_" }

    # ── The PIN, when the host asks for one ─────────────────────────────────
    # A machine this browser has never unlocked shows the login page instead of
    # the library, and every later step then fails on "no element with text
    # 'Display 1'" — which reads like an unpaired host rather than a locked one.
    Start-Sleep -Seconds 8
    [void](Invoke-Unlock)

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
    Say "app ready    : tile '$Tile' is on the page"

    $jsonl = Join-Path $ResultsDir "$Label.jsonl"
    if (Test-Path $jsonl) { Remove-Item $jsonl }

    foreach ($codec in $Codecs) {
        foreach ($enh in @('off', 'on')) {
            $id = "$codec-enhancer-$enh"
            Say ''
            Say "=== $Label / $id ==="
            # Whatever the previous pass left on screen, start from the library.
            if ((Cdp eval "JSON.stringify([...document.querySelectorAll('button')].filter(e=>e.offsetParent).map(e=>e.textContent.trim()))") -match 'Stop Streaming') {
                Cdp exitfs | Out-Null
                Cdp launch 'Stop Streaming' | Out-Null
                Start-Sleep -Seconds 5
            }
            $algo = $enh   # the same word the star matrix writes: 'on' or 'off'
            # "stream_resolution":"fixed" is not decoration: without it the mode
            # stays Auto, the client resolves 1080 against the host's display and
            # writes the host's own height back, and every fleet pass runs at
            # whatever that machine's screen happens to be instead of 1080p.
            $settings = '{"video_codec":"' + $codec + '","stream_resolution":"fixed",' +
                        '"stream_height":1080,"stream_fps":60,' +
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
            # Wait for the stream to actually BE one before measuring it, and keep how
            # long it took: a codec that needs twenty seconds to show its first image is
            # a finding, not a timeout. Until now the pass went straight to fullscreen,
            # and a stream that had not come up yet killed the whole matrix with "no
            # element with text 'Fullscreen'" - which says nothing about the codec.
            $sw = [System.Diagnostics.Stopwatch]::StartNew()
            $live = $false
            for ($t = 0; $t -lt 30; $t++) {
                if ((Cdp stats) -match '"streaming":\s*true') { $live = $true; break }
                Start-Sleep -Seconds 2
            }
            $sw.Stop()
            if (-not $live) {
                Say "  NO STREAM  : nothing came up in $([int]$sw.Elapsed.TotalSeconds)s - recorded and moving on"
                ([pscustomobject]@{ label = $Label; id = $id; asked = "1080p60 $codec 4:2:0 SDR enhancer=$enh";
                                    failed = "the stream never came up" } | ConvertTo-Json -Compress) |
                    Add-Content -Path $jsonl -Encoding UTF8
                continue
            }
            Say "  first image: $([int]$sw.Elapsed.TotalSeconds)s after the click"
            # Fullscreen is cosmetic for these numbers: never fatal.
            try { Cdp fullscreen | Out-Null } catch { Say '  (fullscreen refused - measuring windowed)' }
            Start-Sleep -Seconds $SettleSec

            $row = [ordered]@{
                label      = $Label
                id         = $id
                asked      = "1080p60 $codec 4:2:0 SDR enhancer=$enh"
                negotiated = (Cdp stats).Trim()
            }
            Say "  negotiated : $($row.negotiated)"
            $row.perf = (Cdp perf 15).Trim()
            ([pscustomobject]$row | ConvertTo-Json -Compress -Depth 6) | Add-Content -Path $jsonl -Encoding UTF8

            Cdp exitfs | Out-Null
            if ((Cdp eval "JSON.stringify([...document.querySelectorAll('button')].filter(e=>e.offsetParent).map(e=>e.textContent.trim()))") -match 'Stop Streaming') {
                Cdp launch 'Stop Streaming' | Out-Null
                Start-Sleep -Seconds 5
            }
        }
    }
    Say ''
    Say "fleet half written to $jsonl"
}
finally {
    $Pin = $null
    $script:Pin = $null
    # In this order: the page ends its session (the host revokes it), the kiosk
    # closes, then its profile goes once no Chrome runs on it.
    & python "$PSScriptRoot\owner_kiosk.py" logout --cdp-port $DebugPort | ForEach-Object { Say "$_" }
    if ($LASTEXITCODE -ne 0) {
        $fleetWarnings += 'the session on the host could not be ended by the page: revoke it in its Active Sessions'
    }
    & powershell -NoProfile -File "$PSScriptRoot\kiosk-close.ps1" -Fleet | ForEach-Object { Say "$_" }
    $profilePath = Join-Path $PSScriptRoot $profileName
    for ($i = 0; $i -lt 5 -and (Test-Path -LiteralPath $profilePath); $i++) {
        Remove-Item -LiteralPath $profilePath -Recurse -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $profilePath) { Start-Sleep -Seconds 1 }
    }
    if (Test-Path -LiteralPath $profilePath) {
        $fleetWarnings += "$profileName still holds the host's session cookie: remove it once Chrome is closed"
    }
    Say "fleet kiosk profile removed: $(-not (Test-Path -LiteralPath $profilePath))"
    # The proof no PIN was left: by its shape, in the results and in what this
    # run said. The count only.
    $pinShape = '(?i)\bpin\b["'']?\s*[:=]\s*["'']?\d{4,}'
    $pins = [regex]::Matches(($said -join "`n"), $pinShape).Count
    Get-ChildItem -LiteralPath $ResultsDir -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.Length -lt 64MB } | ForEach-Object {
            $file = $_
            try {
                $pins += [regex]::Matches([System.IO.File]::ReadAllText($file.FullName,
                    [System.Text.Encoding]::GetEncoding(28591)), $pinShape).Count
            } catch { $fleetWarnings += "PINs not counted in $($file.Name): unreadable" }
        }
    Say "PINs left by the run: $pins"
    foreach ($w in $fleetWarnings) { Write-Warning $w }
}
# Said again last, where they cannot be lost in the passes' output.
foreach ($w in $fleetWarnings) { Write-Warning $w }
