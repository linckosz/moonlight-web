# ============================================================================
# The THIRD instrument: what the host made of the key.
#
#   .\keyboard-check.ps1 [-Profiles fr-azerty,us-qwerty] [-WriteSettings]
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
    [switch]   $WriteSettings,
    [switch]   $NoLaunch
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
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
$fidelity = [bool](Get-Prop $settings 'keyboard_layout_fidelity' $true)
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

try {
    # -- A stream, at the reference point: the keys need somewhere to land ----
    if (-not $NoLaunch) {
        if (-not $Tile) {
            $status = Invoke-RestMethod -Uri "$ApiUrl/api/native/status" -Method Get -TimeoutSec 10
            $d = @($status.displays)[0]
            $Tile = Get-Prop $d 'label' ''
            if (-not $Tile) { throw 'cannot name a tile -- pass -Tile' }
        }
        Write-Host "tile         : $Tile"
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
        # host line that may follow it closes it. One line per press, in order,
        # so order is all the pairing needs.
        $records = @()
        $current = $null
        foreach ($line in $kbdLines) {
            if ($line -match '\[KBD\] host ') {
                if ($null -ne $current) { $current.host = $line }
                continue
            }
            if ($line -match "\[KBD\] (\S+) client '(.*?)'( \(non-US\))? -> (.*?) \| Notepad: (.*?) \| Game: (.*)$") {
                $current = [pscustomobject]@{
                    code = $Matches[1]; char = $Matches[2]; nonUs = [bool]$Matches[3]
                    sent = $Matches[4]; wireNote = $Matches[5]; wireGame = $Matches[6]
                    host = ''
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

            # Note: the host's own verdict when it gave one -- it read the key
            # back through its real layout, which no prediction from this side
            # can match. The wire's verdict otherwise.
            $noteText = $rec.wireNote
            if ($rec.host -match '\| Notepad: (.*?) \| Game:') { $noteText = $Matches[1] }
            $row.note = $noteText
            $row.noteOk = [bool]($noteText -match '^OK')

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

        ([pscustomobject]@{
            profile = $profileName; layout = $table.layout; fidelity = $fidelity
            keys = $rows
            summary = [ordered]@{ total = $rows.Count; noteKo = $noteKo.Count
                                  csKo = $csKo.Count; csKoCritical = $csKoCritical.Count }
        } | ConvertTo-Json -Compress -Depth 8) | Add-Content -Path $jsonl -Encoding UTF8
    }

    Write-Host ""
    Write-Host "keyboard verdicts written to $jsonl"
}
finally {
    if ($restore) {
        [System.IO.File]::WriteAllText($SettingsPath, $originalSettings,
                                       (New-Object System.Text.UTF8Encoding $false))
        Write-Host "keyboard_debug: $SettingsPath restored"
    }
}
