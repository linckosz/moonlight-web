# ============================================================================
# MoonlightWeb — bench campaign, step 1: what is actually here today?
#
# Reads hosts.json for the fleet we believe in, probes every machine, and writes
# results/inventory.json with what it FOUND. The campaign matrix is derived from
# that file and from nothing else, which is what makes the campaign adapt to the
# hardware of the day instead of failing on a machine that is switched off.
#
#   powershell -NoProfile -File discover.ps1 [-ResultsDir <path>] [-Quick]
#
# Three traps a bench network sets, all of them handled below:
#
#   - TWO ROUTERS can hand out the same private range. An address answering
#     proves nothing about WHICH machine answered, so the TTL is recorded: a
#     Windows box always replies 128, a Linux one 64. A "Windows" machine
#     answering 64 is a different device, on the other network.
#   - The bench-mini is a DUAL BOOT. The same address is the Wolf + Linux-native
#     bench under Ubuntu and the MultiSeat + AMF bench under Windows, and only a
#     reboot (Bruno's gesture) switches them. The TTL is how we tell.
#   - Every lease is DHCP with no reservation. An address that does not answer
#     is "not found today", never "broken".
# ============================================================================
param(
    [string] $ResultsDir = "$PSScriptRoot\results",
    [switch] $Quick
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null
$fleet = Get-Content "$PSScriptRoot\hosts.json" -Raw -Encoding UTF8 | ConvertFrom-Json
# ConvertFrom-Json hands back a bare object, not an array, when a list holds one
# item — and the committed file holds exactly one machine. Normalise, or the
# first local declaration cannot be appended.
$fleet.machines = @($fleet.machines)

# hosts.json carries the SHAPE of a fleet — the ports a backend answers on, the
# fields a machine entry has, and the one machine every campaign has, the one it
# runs on. A FLEET is declared in hosts.local.json, which is not committed.
#
# The split is not about secrecy alone. A bench fleet is a description of one
# room: these GPUs, this dual boot, that mini PC on a shelf. It is true there and
# false everywhere else, so a repository is the wrong place for it — someone
# cloning this harness wants the machinery, never somebody else's hardware.
#
# Entries are merged on the id: a known id is completed field by field, an
# unknown one is a new machine. Missing fields get harmless defaults so a local
# entry can be as short as an id and an address.
$localFleetPath = "$PSScriptRoot\hosts.local.json"
if (Test-Path $localFleetPath) {
    $localFleet = Get-Content $localFleetPath -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($declared in @($localFleet.machines)) {
        $target = $fleet.machines | Where-Object { $_.id -eq $declared.id } | Select-Object -First 1
        if (-not $target) {
            $target = [pscustomobject]@{
                id    = $declared.id
                label = $declared.id
                kind  = 'ssh'
                roles = @('host')
            }
            $fleet.machines += $target
        }
        foreach ($prop in $declared.PSObject.Properties) {
            if ($prop.Name -eq 'id') { continue }
            $target | Add-Member -NotePropertyName $prop.Name -NotePropertyValue $prop.Value -Force
        }
    }
    # A site can shift a port as well as a machine — Wolf beside a Sunshine on
    # one box is the usual reason — so the local file may carry a "ports" block
    # too, and it wins the same way.
    if ($localFleet.PSObject.Properties.Name -contains 'ports') {
        foreach ($prop in $localFleet.ports.PSObject.Properties) {
            if ($prop.Name -like '_*') { continue }
            $fleet.ports | Add-Member -NotePropertyName $prop.Name -NotePropertyValue $prop.Value -Force
        }
    }
} else {
    Write-Warning "no hosts.local.json — only this machine can be probed. Copy hosts.local.example.json and declare your own fleet in it."
}

# ── Probes ──────────────────────────────────────────────────────────────────

function Test-Port {
    param([string] $Address, [int] $Port, [int] $TimeoutMs = 600)
    $client = New-Object Net.Sockets.TcpClient
    try {
        $async = $client.BeginConnect($Address, $Port, $null, $null)
        if (-not $async.AsyncWaitHandle.WaitOne($TimeoutMs)) { return $false }
        $client.EndConnect($async)
        return $true
    } catch { return $false } finally { $client.Close() }
}

# The TTL of a single echo reply. 128 = Windows, 64 = Linux/macOS. This is the
# dual-boot discriminator and the two-router alarm, so it is worth the parsing.
function Get-PingTtl {
    param([string] $Address)
    $out = & ping.exe -n 1 -w 900 $Address 2>&1 | Out-String
    if ($out -match 'TTL=(\d+)') { return [int]$Matches[1] }
    return $null
}

function Get-OsFromTtl {
    param($Ttl)
    if ($null -eq $Ttl) { return 'unknown' }
    if ($Ttl -ge 100) { return 'windows' }
    return 'unix'
}

# Sunshine's REST API is HTTPS on httpPort+1 with a self-signed certificate, and
# an unauthenticated GET answers 401 naming itself in the realm. That realm is
# the ONLY positive identification available: the GameStream serverinfo is
# identical for Sunshine, Apollo and Wolf, all three impersonating GFE down to
# the version string. Matching the realm rather than the bare 401 is the
# difference between identifying a product and identifying "something that
# wanted a password".
#
# Written on a raw SslStream rather than Invoke-WebRequest for the same reason
# BackendProbe.cpp uses QSslSocket instead of QNetworkAccessManager: the
# high-level client fails this handshake outright ("the underlying connection
# was closed") and reports nothing usable, while the socket gets the 401 every
# time.
function Test-SunshineRest {
    param([string] $Address, [int] $Port, [int] $TimeoutMs = 4000)
    $client = $null; $ssl = $null
    try {
        $client = New-Object Net.Sockets.TcpClient
        $async = $client.BeginConnect($Address, $Port, $null, $null)
        if (-not $async.AsyncWaitHandle.WaitOne($TimeoutMs)) { return $false }
        $client.EndConnect($async)
        $client.ReceiveTimeout = $TimeoutMs
        $client.SendTimeout = $TimeoutMs

        $ssl = New-Object Net.Security.SslStream($client.GetStream(), $false,
                    { param($sender, $cert, $chain, $errors) $true })
        $ssl.AuthenticateAsClient($Address, $null,
            [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls11, $false)

        $request = "GET / HTTP/1.1`r`nHost: ${Address}:${Port}`r`nConnection: close`r`n`r`n"
        $bytes = [Text.Encoding]::ASCII.GetBytes($request)
        $ssl.Write($bytes, 0, $bytes.Length)
        $ssl.Flush()

        $buffer = New-Object byte[] 2048
        $read = $ssl.Read($buffer, 0, $buffer.Length)
        if ($read -le 0) { return $false }
        $head = [Text.Encoding]::ASCII.GetString($buffer, 0, $read)
        return ($head -match 'WWW-Authenticate:[^\r\n]*Sunshine')
    } catch {
        return $false
    } finally {
        if ($ssl) { $ssl.Dispose() }
        if ($client) { $client.Close() }
    }
}

# ── The local machine: the one place we can ask the product itself ──────────
#
# Invoke-RestMethod, but decoded as UTF-8. Windows PowerShell 5.1 falls back to
# ISO-8859-1 whenever a response carries no charset in its Content-Type, so a
# monitor called "M27Q — 2560×1440" came back as "M27Q â€” 2560Ã—1440" and that
# is what landed in inventory.json, then in the report. Read the bytes and
# decode them ourselves.
function Invoke-JsonUtf8 {
    param([string] $Uri, [hashtable] $Headers = @{}, [int] $TimeoutSec = 5)
    $resp = Invoke-WebRequest -Uri $Uri -Headers $Headers -TimeoutSec $TimeoutSec `
                              -UseBasicParsing -ErrorAction Stop
    $bytes = if ($resp.RawContentStream) { $resp.RawContentStream.ToArray() }
             else { [System.Text.Encoding]::UTF8.GetBytes($resp.Content) }
    [System.Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json
}

# /api/native/status is the source of truth for capabilities — displays, live
# HDR state, GPU, encoder, per-codec support — and it answers only a local
# caller. Anything else is guesswork.
function Get-LocalNativeStatus {
    foreach ($port in @(8080, 48080, 49080, 80)) {
        try {
            $token = (Invoke-JsonUtf8 -Uri "http://127.0.0.1:$port/api/admin/token" -TimeoutSec 3).token
            $status = Invoke-JsonUtf8 -Uri "http://127.0.0.1:$port/api/native/status" `
                        -Headers @{ 'X-MW-Admin-Key' = $token }
            $settings = Invoke-JsonUtf8 -Uri "http://127.0.0.1:$port/api/settings/streaming" `
                        -Headers @{ 'X-MW-Admin-Key' = $token }
            return [pscustomobject]@{
                port                = $port
                available           = $status.available
                reason              = $status.reason
                displays            = $status.displays
                capture             = $status.capture
                latencyFlagSupported = $settings.latency_flag_supported
                # The wish (what settings.json says) and the fact (whether the
                # overlay thread is running). A machine where they disagree has
                # been edited without a restart: it will time out every click.
                latencyFlagEnabled  = $settings.latency_flag_enabled
                latencyFlagActive   = $settings.latency_flag_active
                latencyFlagReason   = $settings.latency_flag_reason
                debugBuild          = $settings.debug_build
            }
        } catch { continue }
    }
    return $null
}

# ── Which host software answers on a machine ────────────────────────────────
#
# Mirrors BackendProbe: nothing here NAMES a product from the GameStream reply
# (Sunshine, Apollo and Wolf all impersonate GFE). Sunshine/Apollo give
# themselves away with a 401 + realm on httpPort+1; MultiSeat has a public
# /api/system/auth on 9550; Wolf is an inference — GameStream, no Sunshine REST.
function Get-BackendKinds {
    param([string] $Address, $Ports)
    $kinds = @()
    $gamestream = Test-Port $Address $Ports.gamestreamHttp
    $wolfHttp = Test-Port $Address $Ports.wolfHttp
    if (Test-Port $Address $Ports.multiseatApi) { $kinds += 'multiseat' }

    $sunshineRest = if ($gamestream) { Test-SunshineRest $Address ($Ports.gamestreamHttp + 1) }
                    else { $false }
    if ($sunshineRest) { $kinds += 'sunshine' }

    # Wolf is judged on ITS port and never excluded by Sunshine being there:
    # they are two services, not two guesses about one. Wolf impersonates
    # GameStream and answers on the same pair by default, so a machine running
    # both has one of them rebased — hence a wolfHttp key rather than a constant.
    if ($wolfHttp) { $kinds += 'wolf' }

    # GameStream answering with neither signature is as far as detection goes:
    # it could be Apollo, an unrebased Wolf, or NVIDIA's own. Naming it would be
    # inventing a fact — BackendProbe has the same three-valued answer.
    if ($gamestream -and -not $sunshineRest -and -not $wolfHttp) { $kinds += 'gamestream-unknown' }
    return , $kinds
}

# ── Sweep ───────────────────────────────────────────────────────────────────

$machines = @()
foreach ($m in $fleet.machines) {
    Write-Host "probing $($m.id) ..." -NoNewline

    $entry = [ordered]@{
        id             = $m.id
        label          = $m.label
        kind           = $m.kind
        roles          = $m.roles
        perfMeaningful = $true
        reachable      = $false
        address        = $null
        ttl            = $null
        os             = 'unknown'
        backends       = @()
        native         = $null
        notes          = @()
    }
    if ($m.PSObject.Properties.Name -contains 'perfMeaningful') {
        $entry.perfMeaningful = $m.perfMeaningful
    }

    if ($m.kind -eq 'local') {
        $entry.reachable = $true
        $entry.address = '127.0.0.1'
        $entry.os = 'windows'
        $entry.ttl = 128
        $status = Get-LocalNativeStatus
        if ($status) {
            $entry.native = $status
            $entry.backends += 'native'
            if (-not $status.available) {
                $entry.notes += "native host unavailable: $($status.reason)"
            }
            if (-not $status.latencyFlagSupported) {
                $entry.notes += "no click-to-photon: $($status.latencyFlagReason)"
            }
        } else {
            $entry.notes += 'no MoonlightWeb answering on loopback — start one (--dev) before a campaign'
        }
        $entry.backends += Get-BackendKinds '127.0.0.1' $fleet.ports
        Write-Host " local, backends: $($entry.backends -join ', ')"
        $machines += [pscustomobject]$entry
        continue
    }

    # No address means the machine is declared but hosts.local.json does not say
    # where it is. That is a configuration gap, not a dead machine, and the two
    # deserve different words in the report.
    if ($m.PSObject.Properties.Name -notcontains 'address' -or -not $m.address) {
        $entry.notes += 'no address in hosts.local.json — declared but not reachable from here'
        Write-Host ' no address'
        $machines += [pscustomobject]$entry
        continue
    }
    $address = $m.address
    $entry.address = $address
    $ttl = Get-PingTtl $address
    $entry.ttl = $ttl
    if ($null -eq $ttl) {
        $entry.notes += 'did not answer today — DHCP lease may have moved, or the machine is off'
        Write-Host ' unreachable'
        $machines += [pscustomobject]$entry
        continue
    }

    $entry.reachable = $true
    $entry.os = Get-OsFromTtl $ttl

    # The dual-boot verdict, and the two-router alarm, both fall out of the TTL.
    if ($m.PSObject.Properties.Name -contains 'dualBoot') {
        if ($entry.os -eq 'windows') {
            $entry.notes += 'booted under WINDOWS today: MultiSeat + AMF available, Wolf and the Linux native host are NOT'
        } else {
            $entry.notes += 'booted under LINUX today: Wolf + the Linux native host available, MultiSeat is NOT (Bruno must reboot)'
        }
    } elseif (($m.PSObject.Properties.Name -contains 'expect') -and
              ($m.expect.PSObject.Properties.Name -contains 'os')) {
        $expected = if ($m.expect.os -like 'windows*') { 'windows' } else { 'unix' }
        if ($entry.os -ne $expected) {
            $entry.notes += "TTL says $($entry.os) but this machine should be $expected — very likely ANOTHER device answering on that address; do not trust it"
            $entry.reachable = $false
        }
    }

    if (-not $Quick -and $entry.reachable) {
        $entry.backends = Get-BackendKinds $address $fleet.ports
        if (Test-Port $address 22) { $entry.notes += 'ssh open' }
    }
    Write-Host " ttl=$ttl os=$($entry.os) backends: $($entry.backends -join ', ')"
    $machines += [pscustomobject]$entry
}

# ── The measuring client: this machine's monitors ───────────────────────────
$monitors = @()
try {
    $monitors = & powershell -NoProfile -File "$PSScriptRoot\monitors.ps1"
} catch { }

$inventory = [ordered]@{
    generatedAt = (Get-Date).ToString('o')
    commit      = (& git -C "$PSScriptRoot\..\.." rev-parse --short HEAD 2>$null)
    machines    = $machines
    monitors    = $monitors
}

$path = Join-Path $ResultsDir 'inventory.json'
$inventory | ConvertTo-Json -Depth 8 | Set-Content -Path $path -Encoding UTF8
Write-Host ''
Write-Host "inventory written to $path"

# A campaign with no reachable host is not a failed campaign, it is a campaign
# with nothing to measure — say so plainly rather than producing an empty report.
$live = @($machines | Where-Object { $_.reachable })
if ($live.Count -eq 0) { Write-Warning 'no machine answered — nothing to measure today' }
