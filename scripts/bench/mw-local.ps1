# Ask this machine's running MoonlightWeb one request through its local channel.
# Dot-source it:  . (Join-Path $PSScriptRoot 'mw-local.ps1')
#
#   $r = Invoke-MwLocalRequest 'GET /api/native/status' -Port 8443
#   $r.Status   # 200, or 0 when no status came back
#   $r.Json     # the answer, parsed (2xx only: a refusal's body is not shown)
#
# The server's own command does it: <exe> [--dev] --local-request "<METHOD> <path>",
# a body on standard input for a POST. The exe and its --dev are those of the
# instance that listens on the port: the channel is that instance's own.
#
# An exe that does not have the command is never started with it: it would refuse
# the option in a dialog, in front of whoever sits at the machine. Whether it has
# it is read from its bytes (Test-MwLocalRequest), never by starting it.
#
# Nothing here prints what it sends or what it gets: a PIN asked for stays in the
# caller's variable. No -Verbose, no transcript around a call that carries one.

$script:MwLocalKnown = @{}

function Get-MwInstance([int]$Port) {
    $c = Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $c) { return $null }
    $p = Get-CimInstance Win32_Process -Filter "ProcessId=$($c.OwningProcess)"
    if (-not $p -or -not $p.ExecutablePath) { return $null }
    [pscustomobject]@{ Exe = $p.ExecutablePath; Dev = [bool]($p.CommandLine -match '(^|\s)--dev(\s|$)') }
}

# The names the server's exe goes by: whatever else listens on a port is never
# started with an argument, whatever its bytes hold.
$script:MwServerNames = @('moonlightweb.exe', 'moonlightweb', 'moonlight-web', 'moonlightwebdev.exe',
                          'moonlightwebdev', 'moonlightweb-dev')

function Test-MwLocalRequest([string]$Exe) {
    if ($script:MwServerNames -notcontains ([System.IO.Path]::GetFileName($Exe)).ToLowerInvariant()) { return $false }
    $item = Get-Item -LiteralPath $Exe -ErrorAction SilentlyContinue
    if (-not $item) { return $false }
    $key = '{0}|{1}|{2}' -f $item.FullName, $item.Length, $item.LastWriteTimeUtc.Ticks
    if (-not $script:MwLocalKnown.ContainsKey($key)) {
        $bytes = [System.IO.File]::ReadAllBytes($item.FullName)
        $ascii = [System.Text.Encoding]::ASCII.GetBytes('local-request')
        $wide = [System.Text.Encoding]::Unicode.GetBytes('local-request')
        $script:MwLocalKnown[$key] = (Find-MwBytes $bytes $ascii) -or (Find-MwBytes $bytes $wide)
    }
    $script:MwLocalKnown[$key]
}

function Find-MwBytes([byte[]]$Haystack, [byte[]]$Needle) {
    $first = $Needle[0]
    $i = [Array]::IndexOf($Haystack, $first)
    while ($i -ge 0 -and $i -le $Haystack.Length - $Needle.Length) {
        $j = 1
        while ($j -lt $Needle.Length -and $Haystack[$i + $j] -eq $Needle[$j]) { $j++ }
        if ($j -eq $Needle.Length) { return $true }
        $i = [Array]::IndexOf($Haystack, $first, $i + 1)
    }
    $false
}

# A read (GET) of the instance behind an API URL of this machine: through its local
# channel when its exe has the command (the address alone no longer opens these
# reads), by the address otherwise, which an older build still answers.
function Get-MwLocalApi([string] $ApiUrl, [string] $Path, [int] $TimeoutSec = 10) {
    $found = Get-MwInstance ([uri]$ApiUrl).Port
    if ($found -and (Test-MwLocalRequest $found.Exe)) {
        $r = Invoke-MwLocalRequest "GET $Path" -Exe $found.Exe -Dev:$found.Dev -TimeoutSec ([Math]::Max($TimeoutSec, 30))
        if ($r.Status -ne 200) { throw "GET $Path answered $($r.Status) on the local channel" }
        return $r.Json
    }
    Invoke-RestMethod -Uri ($ApiUrl.TrimEnd('/') + $Path) -Method Get -TimeoutSec $TimeoutSec
}

function Invoke-MwLocalRequest {
    param(
        [Parameter(Mandatory = $true, Position = 0)][string]$Request,
        [int]$Port = 0,
        [string]$Exe = '',
        [switch]$Dev,
        [string]$Body = $null,
        [int]$TimeoutSec = 60
    )
    $devFlag = [bool]$Dev
    if (-not $Exe) {
        $found = Get-MwInstance $Port
        if (-not $found) { throw "local request: nothing of this machine listens on port $Port" }
        $Exe = $found.Exe; $devFlag = $found.Dev
    }
    if (-not (Test-MwLocalRequest $Exe)) { throw "local request: this instance's exe has no --local-request (not started)" }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $quoted = '"' + ($Request -replace '"', '') + '"'
    $psi.Arguments = $(if ($devFlag) { '--dev ' } else { '' }) + '--local-request ' + $quoted
    $psi.WorkingDirectory = Split-Path -Parent $Exe
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $p = [System.Diagnostics.Process]::Start($psi)
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    if ($Body) {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Body)
        $p.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
    }
    $p.StandardInput.Close()
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        try { $p.Kill() } catch { }
        throw "local request: no answer in $TimeoutSec s"
    }
    $status = 0
    foreach ($line in ($errTask.Result -split "`r?`n")) {
        if ($line.Trim() -match '^HTTP (\d{3})$') { $status = [int]$Matches[1] }
    }
    $text = $outTask.Result
    $json = $null
    if ($text.Trim()) { try { $json = $text | ConvertFrom-Json } catch { } }
    [pscustomobject]@{ Status = $status; Text = $text; Json = $json }
}
