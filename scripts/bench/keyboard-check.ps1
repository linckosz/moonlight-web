# ============================================================================
# The THIRD instrument: what the host made of the key.
#
#   .\keyboard-check.ps1 [-Profiles fr-azerty,us-qwerty] [-WriteSettings]
#   .\keyboard-check.ps1 -OwnerLink -ClientRect x,y,w,h [-AppUrl ...]
#
# The other two instruments measure how fast a picture comes back. This one
# measures whether the right thing happened at all, and it exists because a
# keystroke has TWO jobs that fail separately and are never both visible at
# once:
#
#   Note  the character a text field shows. What a typist sees, and what
#         layout fidelity was built to get right.
#   CS    the physical key a game reading the raw keyboard sees, named by its
#         US label. Weapon slots 1-5 and the movement keys live here, and
#         nothing about them cares which character the key carries.
#
# They pull in opposite directions, which is the entire point of measuring
# them. Injecting Unicode types the character perfectly and no game ever knows
# a key was pressed. Pressing the key that CARRIES the character on the host's
# own layout keeps the key real, but on an AZERTY client that key is not the
# one the player pressed: weapon slot 1 is typed as & and answers as 7.
#
# The host says which key it pressed; the bench decides whether that was the
# right one for a game. No log line can make that call on its own, because
# only the bench knows which POSITION was pressed at the other end.
#
# The client layout is SIMULATED, not installed: cdp.py dispatches a position
# and a character independently, so an AZERTY client is replayed from a machine
# that runs anything, identically on every bench of the fleet, with nobody
# logging out to switch a keyboard. Its limit is AltGr, which the CDP modifier
# bitmask cannot express -- so no table lists an AltGr key.
#
# Requires "keyboard_debug": true in the host's settings.json. It is an
# instrument, not a preference: it is not seeded, and this script will not
# write it into a production configuration unless -WriteSettings says so, in
# which case the file is restored byte for byte on the way out.
# ============================================================================
param(
    [string]   $ResultsDir = '',
    [string[]] $Profiles = @('fr-azerty', 'us-qwerty'),
    [string]   $ApiUrl = 'http://127.0.0.1:8080',
    [string]   $Tile = '',
    [string]   $LogDir = '',
    [string]   $SettingsPath = '',
    [int]      $DebugPort = 9333,
    # Two presses of one key closer than this are one keystroke come back.
    [int]      $EchoWindowMs = 60,
    # More presses than this for one keystroke is a loop, flagged as such.
    [int]      $FloodPresses = 3,
    [switch]   $WriteSettings,
    [switch]   $NoLaunch,
    # Without it, the keys go to the Chrome already listening on -DebugPort, with
    # the app open and its session held. With it, this script opens that client
    # kiosk itself on -ClientRect and gives it the host's session as its owner
    # does, by the link --print-link makes (owner_kiosk.py, as run-browser.ps1
    # -OwnerLink): a bench instance only (--dev or the DEV edition), never the
    # production one. Its own profile (.chrome-owner); the session ends with the
    # run: the page logs out, the kiosk closes, the profile goes, whatever
    # happened. While it runs, the kiosk's DevTools port (127.0.0.1) hands any
    # process of this machine a page that holds the host's session: never left
    # running unattended on a shared machine.
    [switch]   $OwnerLink,
    [string]   $AppUrl = 'https://127.0.0.1:8443/',
    # x,y,w,h of the client kiosk, as run-browser.ps1 -ClientRect.
    [string]   $ClientRect = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($OwnerLink -and $NoLaunch) {
    throw '-OwnerLink with -NoLaunch: the kiosk this script opens has no stream of its own to type into'
}
if ($OwnerLink -and @($ClientRect -split ',').Count -ne 4) {
    throw '-OwnerLink needs -ClientRect x,y,w,h: where the client kiosk goes'
}
if (-not $ResultsDir) { $ResultsDir = Join-Path $PSScriptRoot 'results' }
if (-not (Test-Path $ResultsDir)) { New-Item -ItemType Directory -Path $ResultsDir | Out-Null }
$appData = Join-Path $env:APPDATA 'MoonlightWeb\MoonlightWeb'
if (-not $SettingsPath) { $SettingsPath = Join-Path $appData 'settings.json' }
if (-not $LogDir) { $LogDir = Join-Path $appData 'logs' }
Set-Location $PSScriptRoot

function Cdp {
    param([Parameter(ValueFromRemainingArguments = $true)] $Args)
    & python "$PSScriptRoot\cdp.py" --port $DebugPort @Args 2>&1 | Out-String
}

function Get-Prop {
    param($Object, [string] $Name, $Default = $null)
    if ($null -eq $Object) { return $Default }
    if ($Object.PSObject.Properties.Name -contains $Name) { return $Object.$Name }
    return $Default
}

# -- The instrument has to be armed on the host, and armed is not the default -
if (-not (Test-Path $SettingsPath)) {
    throw "no settings.json at $SettingsPath -- pass -SettingsPath"
}
$originalSettings = [System.IO.File]::ReadAllText($SettingsPath)
$settings = $originalSettings | ConvertFrom-Json
# "auto" (the default), "always" or "never" since 22/10/2026; a boolean from
# 0.4.0 or before reads as the server reads it: true "always", false "auto".
$fidelityRaw = Get-Prop $settings 'keyboard_layout_fidelity' 'auto'
$fidelity = if ($fidelityRaw -is [bool]) { if ($fidelityRaw) { 'always' } else { 'auto' } } else { "$fidelityRaw" }
$debugOn = [bool](Get-Prop $settings 'keyboard_debug' $false)
$restore = $false

if (-not $debugOn) {
    if (-not $WriteSettings) {
        throw @"
keyboard diagnostics are off on this host, and this script will not turn them on by itself.
Add to $SettingsPath :

    "keyboard_debug": true

The next stream picks it up -- the worker reads the file when it starts, so no restart is
needed. Take it out when the campaign is over. Or re-run with -WriteSettings and the file
is put back exactly as it was on the way out.
"@
    }
    $settings | Add-Member -NotePropertyName 'keyboard_debug' -NotePropertyValue $true -Force
    # No BOM: QJsonDocument::fromJson rejects a file that starts with one, and
    # PowerShell's own -Encoding UTF8 writes one.
    [System.IO.File]::WriteAllText($SettingsPath, ($settings | ConvertTo-Json -Depth 20),
                                   (New-Object System.Text.UTF8Encoding $false))
    $restore = $true
    Write-Host "keyboard_debug: armed in $SettingsPath (will be restored)"
}

Write-Host "settings     : keyboard_layout_fidelity = $fidelity"
Write-Host "profiles     : $($Profiles -join ', ')"

$ownerWarnings = @()
try {
    # -- -OwnerLink: the client kiosk, and the owner's session in it ----------
    # Inside the try: from the moment the kiosk exists, the finally ends it.
    if ($OwnerLink) {
        $r = $ClientRect -split ','
        # On a blank page first: it goes to the app once the link has opened its session.
        & powershell -NoProfile -File "$PSScriptRoot\kiosk.ps1" -Url 'about:blank' `
            -X $r[0] -Y $r[1] -W $r[2] -H $r[3] -DebugPort $DebugPort -ChromeProfile '.chrome-owner' | Out-Host
        # The link goes from the instance's exe to the kiosk through a pipe, and
        # nowhere else; this prints yes/no only (owner_kiosk.py).
        $api = [uri]$ApiUrl
        $app = [uri]$AppUrl
        & python "$PSScriptRoot\owner_kiosk.py" open --api-port $api.Port --cdp-port $DebugPort `
            --origin "$($app.Scheme)://$($app.Authority)" --lan-only | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "the owner's link did not open the client kiosk's session (owner_kiosk.py open)" }
        Cdp nav $AppUrl | Out-Null
    }

    # -- A stream, at the reference point: the keys need somewhere to land ----
    if (-not $NoLaunch) {
        if (-not $Tile) {
            # Through the host's local channel when its exe has the command.
            . "$PSScriptRoot\mw-local.ps1"
            $status = Get-MwLocalApi $ApiUrl '/api/native/status'
            $d = @($status.displays)[0]
            $Tile = Get-Prop $d 'label' ''
            if (-not $Tile) { throw 'cannot name a tile -- pass -Tile' }
        }
        Write-Host "tile         : $Tile"
        if ($OwnerLink) {
            # A cold Chrome on a single-page app: the tile exists only once the
            # host list has arrived (run-browser.ps1, Wait-Tile).
            $seen = $false
            for ($i = 0; $i -lt 40 -and -not $seen; $i++) {
                $seen = (Cdp eval "JSON.stringify(document.body ? document.body.innerText : '')") -match [regex]::Escape($Tile)
                if (-not $seen) { Start-Sleep -Seconds 2 }
            }
            if (-not $seen) { throw "the app at $AppUrl never showed a tile named '$Tile'" }
        }
        Cdp launch $Tile | Out-Null
        Start-Sleep -Seconds 3
        if ((Cdp eval "!!document.querySelector('.self-stream-go')") -match 'True|true') {
            Cdp launch 'Stream anyway' | Out-Null
            Start-Sleep -Seconds 8
        }
        if ((Cdp eval "!!document.querySelector('canvas')") -notmatch 'True|true') {
            throw 'the stream never started -- nothing would receive a key'
        }
    }

    # The keys are handled by the STREAM WORKER, which writes its own log file:
    # the parent's moonlightweb.log carries not one [KBD] line, and looking
    # there is the quickest way to conclude the instrument is broken when it is
    # simply somewhere else.
    $workerLog = Get-ChildItem (Join-Path $LogDir 'moonlightweb-worker-*.log') -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $workerLog) { throw "no worker log in $LogDir -- is the stream really running?" }
    Write-Host "worker log   : $($workerLog.Name)"

    $jsonl = Join-Path $ResultsDir 'keyboard.jsonl'
    if (Test-Path $jsonl) { Remove-Item $jsonl }

    foreach ($profileName in $Profiles) {
        $tablePath = Join-Path $PSScriptRoot "keyboard\$profileName.json"
        if (-not (Test-Path $tablePath)) { throw "no key table at $tablePath" }
        $table = Get-Content $tablePath -Raw -Encoding UTF8 | ConvertFrom-Json

        Write-Host ""
        Write-Host "=== $profileName ($($table.layout)) ==="

        $mark = (Get-Item $workerLog.FullName).Length
        Cdp keys $tablePath | Out-Null
        Start-Sleep -Seconds 2

        # Read from the mark: everything the worker logged while we typed.
        $fs = [System.IO.File]::Open($workerLog.FullName, 'Open', 'Read', 'ReadWrite')
        try {
            $fs.Seek($mark, 'Begin') | Out-Null
            $reader = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::UTF8)
            $tail = $reader.ReadToEnd()
        } finally { $fs.Dispose() }
        $kbdLines = @($tail -split "`r?`n" | Where-Object { $_ -match '\[KBD\]' })

        if ($kbdLines.Count -eq 0) {
            Write-Warning "  not one [KBD] line -- diagnostics are off in the process that handles keys"
            ([pscustomobject]@{ profile = $profileName; fidelity = $fidelity
                                error = 'no [KBD] line in the worker log' } |
                ConvertTo-Json -Compress) | Add-Content -Path $jsonl -Encoding UTF8
            continue
        }

        # Pair the lines with the keys: a transport line opens a record, the
        # host line that may follow it closes it, in order.
        #
        # A press that reaches the host within a few milliseconds of the
        # previous one is the SAME keystroke coming back, not the next key
        # (cdp.py types them 120 ms apart): folded into its record and counted. On a
        # self-stream (this bench streams its own screen) the host injects into
        # the foreground window, which is the client, and a key CDP pressed was
        # never down for the OS -- so it comes back once, as a fresh press, and
        # under another code when the host resolved the character to another
        # key (us-qwerty 'w' on an AZERTY host comes back as KeyZ). More than
        # $FloodPresses is a loop: until 25/09/2026 every key came back
        # forever, every 22 ms, and the table read "log out of step" from the
        # second key on.
        $records = @()
        $lastT = [datetime]::MinValue
        $current = $null
        foreach ($line in $kbdLines) {
            if ($line -match '\[KBD\] host ') {
                if ($null -ne $current -and -not $current.host) { $current.host = $line }
                continue
            }
            if ($line -match "^\[(\S+ \S+)\].*\[KBD\] (\S+) client '(.*?)'( \(non-US\))? -> (.*?) \| Notepad: (.*?) \| Game: (.*)$") {
                $t = [datetime]::ParseExact($Matches[1], 'yyyy-MM-dd HH:mm:ss.fff', $null)
                $echo = $null -ne $current -and ($t - $lastT).TotalMilliseconds -lt $EchoWindowMs
                $lastT = $t
                if ($echo) {
                    $current.presses++
                    continue
                }
                $current = [pscustomobject]@{
                    code = $Matches[2]; char = $Matches[3]; nonUs = [bool]$Matches[4]
                    sent = $Matches[5]; wireNote = $Matches[6]; wireGame = $Matches[7]
                    host = ''; presses = 1
                }
                $records += $current
            }
        }

        # -- The two verdicts ------------------------------------------------
        $rows = @()
        $i = 0
        foreach ($k in $table.keys) {
            $rec = if ($i -lt $records.Count) { $records[$i] } else { $null }
            $i++
            $row = [ordered]@{
                code = $k.code; key = $k.key; shift = [bool]$k.shift; us = $k.us
                role = $k.role; why = $k.why
            }
            if ($null -eq $rec) {
                $row.error = 'no [KBD] line for this key'
                $rows += [pscustomobject]$row
                continue
            }
            if ($rec.code -ne $k.code) {
                # Out of step: one key logged nothing (a non-printable slipped
                # into the table) and every verdict after it would be about the
                # wrong key. Say so rather than report a shifted table.
                $row.error = "log out of step: expected $($k.code), read $($rec.code)"
                $rows += [pscustomobject]$row
                continue
            }
            $row.sent = $rec.sent
            $row.hostSaid = $rec.host
            $row.presses = $rec.presses
            if ($rec.presses -gt $FloodPresses) { $row.flood = $true }

            # Note: the host's own verdict when it gave one -- it read the key
            # back through its real layout, which no prediction from this side
            # can match. The wire's verdict otherwise.
            $noteText = $rec.wireNote
            if ($rec.host -match '\| Notepad: (.*?) \| Game:') { $noteText = $Matches[1] }
            $row.note = $noteText
            $row.noteOk = [bool]($noteText -match '^OK')
            # Positional mode (keyboard_layout_fidelity "never", or gaming mode under "auto")
            # sends no character, so the host has nothing to check against - but
            # it says what its layout typed for that position, and this side knows
            # what the client's layout meant: compare the two. The host names the
            # UNSHIFTED character; a shifted key cannot be judged from that line.
            if ($noteText -match '^no client character' -and $rec.host -match "this layout types '(.*?)'") {
                $typed = $Matches[1]
                if ($row.shift) { $row.noteOk = $null; $row.note = "not judged: the host names the unshifted '$typed'" }
                else {
                    $row.noteOk = ($typed -ceq $row.key)
                    $row.note = if ($row.noteOk) { "OK typed '$typed'" } else { "KO typed '$typed', the client meant '$($row.key)'" }
                }
            }

            # CS: which physical key was really pressed, named in US. The host
            # names it; only we know which position was pressed, so only we can
            # say whether they are the same key.
            $gameText = $rec.wireGame
            if ($rec.host -match '\| Game: (.*)$') { $gameText = $Matches[1] }
            $row.game = $gameText
            if ($gameText -match "US '(.*?)'") {
                $pressed = $Matches[1]
                $row.gameKey = $pressed
                $row.gameOk = ($pressed.ToUpper() -eq ([string]$k.us).ToUpper())
            } else {
                $row.gameKey = ''
                $row.gameOk = -not ($gameText -match '^KO')
            }
            $rows += [pscustomobject]$row
        }

        $noteKo = @($rows | Where-Object { (Get-Prop $_ 'noteOk' $true) -eq $false })
        $csKo = @($rows | Where-Object { (Get-Prop $_ 'gameOk' $true) -eq $false })
        $csKoCritical = @($csKo | Where-Object { $_.role -eq 'cs' -or $_.role -eq 'both' })
        $flooded = @($rows | Where-Object { Get-Prop $_ 'flood' $false })
        $maxPresses = ($rows | ForEach-Object { Get-Prop $_ 'presses' 0 } | Measure-Object -Maximum).Maximum

        foreach ($r in $rows) {
            $n = if ($null -eq (Get-Prop $r 'noteOk' $null)) { '??' }
                 elseif ($r.noteOk) { 'OK' } else { 'KO' }
            $g = if ($null -eq (Get-Prop $r 'gameOk' $null)) { '??' }
                 elseif ($r.gameOk) { 'OK' } else { 'KO' }
            $pressed = Get-Prop $r 'gameKey' ''
            if (-not $pressed) { $pressed = '-' }
            Write-Host ("  {0,-14} '{1}'  Note {2}  CS {3}  (US {4} -> {5})" -f `
                $r.code, $r.key, $n, $g, $r.us, $pressed)
        }
        Write-Host ("  -> Note KO: {0}/{1}   CS KO: {2}/{1}   of which game-critical: {3}" -f `
            $noteKo.Count, $rows.Count, $csKo.Count, $csKoCritical.Count)
        if ($flooded.Count) {
            Write-Warning ("  {0} key(s) reached the host more than {1} times per keystroke (up to {2}): an input loop" -f `
                $flooded.Count, $FloodPresses, $maxPresses)
        }

        ([pscustomobject]@{
            profile = $profileName; layout = $table.layout; fidelity = $fidelity
            keys = $rows
            summary = [ordered]@{ total = $rows.Count; noteKo = $noteKo.Count
                                  csKo = $csKo.Count; csKoCritical = $csKoCritical.Count
                                  flooded = $flooded.Count; maxPresses = $maxPresses }
        } | ConvertTo-Json -Compress -Depth 8) | Add-Content -Path $jsonl -Encoding UTF8
    }

    Write-Host ""
    Write-Host "keyboard verdicts written to $jsonl"
}
finally {
    if ($OwnerLink) {
        # In this order: the stream stops, the page ends its session (the host
        # revokes it), the kiosk closes, then the profile goes once no Chrome
        # runs on it. Nothing here may throw before the profile is gone: cdp.py's
        # stderr through 2>&1 is a terminating error under 'Stop' in PowerShell 5.1
        # (a kiosk that never opened, a page already gone).
        try {
            $btns = Cdp eval "JSON.stringify([...document.querySelectorAll('button')].filter(e=>e.offsetParent).map(e=>e.textContent.trim()))"
            if ($btns -match 'Stop Streaming') {
                Cdp exitfs | Out-Null
                Cdp launch 'Stop Streaming' | Out-Null
                Start-Sleep -Seconds 4
            }
        } catch { Write-Host 'stream stop: the page did not answer' }
        & python "$PSScriptRoot\owner_kiosk.py" logout --cdp-port $DebugPort | Out-Host
        if ($LASTEXITCODE -ne 0) {
            $ownerWarnings += 'the owner session could not be ended by the page: revoke it in Active Sessions'
        }
        & powershell -NoProfile -File "$PSScriptRoot\kiosk-close.ps1" -Client | Out-Host
        for ($i = 0; $i -lt 5; $i++) {
            $cleaned = & python "$PSScriptRoot\owner_kiosk.py" clean
            if ($LASTEXITCODE -eq 0) { break }
            Start-Sleep -Seconds 1
        }
        $cleaned | Out-Host
        if ($LASTEXITCODE -ne 0) {
            $ownerWarnings += ".chrome-owner still holds the owner's session cookie: remove it once Chrome is closed"
        }
        # The proof the run wrote no key: how many times "#k=", "&k=" or "?k="
        # appear in what it left in the results. The count only.
        $kHits = 0
        Get-ChildItem -LiteralPath $ResultsDir -File -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.Length -lt 64MB } | ForEach-Object {
                $file = $_
                try {
                    $text = [System.IO.File]::ReadAllText($file.FullName, [System.Text.Encoding]::GetEncoding(28591))
                    $kHits += [regex]::Matches($text, '[#&?]k=').Count
                } catch { $ownerWarnings += "k= not counted in $($file.Name): unreadable" }
            }
        Write-Host "owner link: k= in the results: $kHits"
        # Here too: after a throw in the try, the end of the script is never reached.
        foreach ($w in $ownerWarnings) { Write-Warning $w }
    }
    if ($restore) {
        [System.IO.File]::WriteAllText($SettingsPath, $originalSettings,
                                       (New-Object System.Text.UTF8Encoding $false))
        Write-Host "keyboard_debug: $SettingsPath restored"
    }
}
# Said again last, where it cannot be lost in the keys' output.
foreach ($w in $ownerWarnings) { Write-Warning $w }
