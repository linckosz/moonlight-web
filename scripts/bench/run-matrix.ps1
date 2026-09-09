# ============================================================================
# The encoder half of a campaign: --native-bench, one pass per spec.
#
# --native-bench captures, converts and encodes a display into a sink — no
# network, no browser — and writes one CSV row per frame. It is the only
# instrument that isolates the encoder from everything else, it runs on every
# platform, and a pass costs ten seconds. Every campaign starts here, and the
# browser passes then say what the rest of the chain adds.
#
#   .\run-matrix.ps1 -Display 1 -Specs @('codec=hevc','codec=h264','codec=av1')
#                    [-Base 'seconds=10,bitrate=20000']
#                    [-Exe ..\..\build\MoonlightWeb.exe]
#                    [-Relaunch -ContentUrl <url> -KioskRect '<x,y,w,h>']
#                    [-ResultsDir <path>]
#
# -Relaunch restarts the content kiosk before every pass and waits -SettleMs, so
# each pass encodes the SAME seconds of the same footage. Without it, comparing
# two settings compares two different explosions.
#
# The statistics are computed from the per-frame CSV, never scraped from the
# summary text: the columns are a contract (NativeBench.cpp), the prose is not.
# ============================================================================
param(
    [Parameter(Mandatory = $true)] [int] $Display,
    # A spec is itself comma-separated ("codec=hevc,fps=60"), and `powershell
    # -File` flattens an array argument into positional ones — so a caller in
    # another process passes -SpecFile, one spec per line, and only an in-process
    # caller uses -Specs.
    [string[]] $Specs,
    [string] $SpecFile,
    [string] $Base = 'seconds=10,bitrate=20000',
    [string] $Exe = "$PSScriptRoot\..\..\build\MoonlightWeb.exe",
    [string] $ResultsDir = "$PSScriptRoot\results",
    [switch] $Relaunch,
    [string] $ContentUrl,
    [string] $KioskRect = '',
    [int] $SettleMs = 3500
)

$ErrorActionPreference = 'Stop'
if ($SpecFile) {
    if (-not (Test-Path $SpecFile)) { throw "no spec file at $SpecFile" }
    $Specs = @(Get-Content $SpecFile -Encoding UTF8 | Where-Object { $_.Trim() })
}
if (-not $Specs -or $Specs.Count -eq 0) { throw "give -Specs or -SpecFile" }
if (-not (Test-Path $Exe)) { throw "no MoonlightWeb.exe at $Exe — build it first" }
$csvDir = Join-Path $ResultsDir 'native-bench'
New-Item -ItemType Directory -Force -Path $csvDir | Out-Null

function Get-Percentile {
    param([double[]] $Values, [double] $P)
    if ($Values.Count -eq 0) { return $null }
    $sorted = $Values | Sort-Object
    $i = [Math]::Min($sorted.Count - 1, [Math]::Max(0, [Math]::Ceiling($P * $sorted.Count) - 1))
    return [double]$sorted[$i]
}

# Every row has the SAME shape, always. Export-Csv takes its header from the
# FIRST object it is given and silently drops any property the later ones add —
# so a failed pass whose object carried only `spec` and `error` came out as a
# row of empty cells, and the failure vanished from the campaign. A bench that
# loses its failures is worse than no bench.
function New-Row {
    param([string] $Spec)
    [ordered]@{
        spec = $Spec; error = ''; negotiated = ''; csv = ''
        frames = $null; keyframes = $null; captureFps = $null
        encodeMean = $null; encodeP95 = $null; encodeP99 = $null
        totalMean = $null; totalP99 = $null
        deltaKB = $null; deltaKBp95 = $null; avgQp = $null
    }
}

function Measure-Pass {
    param([string] $CsvPath)
    $rows = Import-Csv -Path $CsvPath
    # Re-sent frames carry no capture and would drag every average down: the
    # engine stamps present, captured and submitted with the same instant for
    # them, and the bench flags that as captured=0.
    $captured = @($rows | Where-Object { $_.captured -eq '1' })
    if ($captured.Count -eq 0) { return $null }
    $deltas = @($captured | Where-Object { $_.keyframe -eq '0' })

    $encode = [double[]]@($captured | ForEach-Object { [double]$_.encode_us / 1000.0 })
    $total = [double[]]@($captured | ForEach-Object { [double]$_.host_total_us / 1000.0 })
    $bytes = [double[]]@($deltas | ForEach-Object { [double]$_.bytes })
    $qp = [double[]]@($captured | Where-Object { [double]$_.avg_qp -gt 0 } |
            ForEach-Object { [double]$_.avg_qp })

    # Presents over the span BETWEEN presents: n-1 intervals, not n, and
    # measured on the present stamps alone. Mixing t0 with the last t3 would
    # fold one frame's encode time into the period and quietly lower the rate.
    $spanUs = [double]$captured[-1].t0_present_us - [double]$captured[0].t0_present_us
    $fps = if ($spanUs -gt 0) { ($captured.Count - 1) / ($spanUs / 1e6) } else { $null }

    return [ordered]@{
        frames      = $captured.Count
        keyframes   = @($captured | Where-Object { $_.keyframe -eq '1' }).Count
        captureFps  = if ($fps) { [math]::Round($fps, 1) } else { $null }
        encodeMean  = [math]::Round(($encode | Measure-Object -Average).Average, 2)
        encodeP95   = [math]::Round((Get-Percentile $encode 0.95), 2)
        encodeP99   = [math]::Round((Get-Percentile $encode 0.99), 2)
        totalMean   = [math]::Round(($total | Measure-Object -Average).Average, 2)
        totalP99    = [math]::Round((Get-Percentile $total 0.99), 2)
        deltaKB     = if ($bytes.Count) { [math]::Round(($bytes | Measure-Object -Average).Average / 1024, 1) } else { $null }
        deltaKBp95  = if ($bytes.Count) { [math]::Round((Get-Percentile $bytes 0.95) / 1024, 1) } else { $null }
        avgQp       = if ($qp.Count) { [math]::Round(($qp | Measure-Object -Average).Average, 1) } else { $null }
    }
}

$results = @()
$index = 0
foreach ($spec in $Specs) {
    $index++
    $label = ($spec -replace '[^A-Za-z0-9=,]', '') -replace '[=,]', '-'
    $csv = Join-Path $csvDir "pass-$index-$label.csv"
    $full = "display=$Display,$Base,$spec,out=$csv"

    if ($Relaunch -and $ContentUrl) {
        if (-not $KioskRect) { throw "-Relaunch needs -KioskRect '<x,y,w,h>'; run-campaign derives it from the captured display" }
        $r = $KioskRect -split ','
        & powershell -NoProfile -File "$PSScriptRoot\kiosk.ps1" -Url $ContentUrl `
            -X ([int]$r[0]) -Y ([int]$r[1]) -W ([int]$r[2]) -H ([int]$r[3]) | Out-Null
        Start-Sleep -Milliseconds $SettleMs
    }

    Write-Host "[$index/$($Specs.Count)] $spec"
    # Start-Process, not `& $Exe`: Windows PowerShell 5.1 turns EVERY line a
    # native command writes to stderr into an ErrorRecord — with or without a
    # redirection — and under $ErrorActionPreference='Stop' the matrix then dies
    # on the engine's first informational line ("AMF confirmed ..."). Redirecting
    # through Start-Process keeps both streams as plain files, which is also
    # where they are wanted when a pass has to be explained afterwards.
    $errPath = [IO.Path]::ChangeExtension($csv, '.err')
    $outPath = [IO.Path]::ChangeExtension($csv, '.out')
    Start-Process -FilePath $Exe -ArgumentList @('--native-bench', $full) `
        -NoNewWindow -Wait -RedirectStandardOutput $outPath -RedirectStandardError $errPath
    # -Encoding UTF8 for the same reason as the stderr read below: the engine
    # writes UTF-8, and without it Get-Content decodes as ANSI and the middot
    # of the negotiated line lands in the report as a double-encoded 'A-tilde'.
    $stdout = if (Test-Path $outPath) { Get-Content $outPath -Raw -Encoding UTF8 } else { '' }

    $row = New-Row $spec
    $row.csv = $csv

    if (-not (Test-Path $csv)) {
        # The line that EXPLAINS, not the last four lines: the engine logs a
        # dozen informational lines before it gives up, and quoting the tail
        # puts "colour conversion: ..." in the report where the driver's refusal
        # belongs. -Encoding UTF8 because the engine writes UTF-8 and Get-Content
        # would otherwise read it as ANSI and mangle every dash.
        $errLines = if (Test-Path $errPath) { @(Get-Content $errPath -Encoding UTF8) } else { @() }
        $said = @($errLines | Where-Object {
            $_ -match 'could not|failed|unsupported|not supported|refused|error'
        })
        $tail = if ($said.Count) { ($said | Select-Object -Last 2) -join ' ' }
                elseif ($errLines.Count) { ($errLines | Select-Object -Last 2) -join ' ' }
                else { ($stdout -split "`n" | Select-Object -Last 2) -join ' ' }
        Write-Warning "  no CSV produced — the pass failed: $tail"
        $row.error = $tail.Trim()
        $results += [pscustomobject]$row
        continue
    }

    $stats = Measure-Pass $csv
    if (-not $stats) {
        Write-Warning "  the pass produced no captured frame"
        $row.error = 'no captured frame'
        $results += [pscustomobject]$row
        continue
    }
    # The engine's own description of what it really did — encoder, codec,
    # chroma and HDR as NEGOTIATED, which is not always what was asked for.
    $negotiated = ($stdout -split "`n" | Where-Object { $_ -match '^native-bench: ' } |
                   Select-Object -First 1) -replace '^native-bench: ', ''
    $row.negotiated = $negotiated.Trim()
    foreach ($k in $stats.Keys) { $row[$k] = $stats[$k] }
    $results += [pscustomobject]$row
    Write-Host ("  encode {0} / {1} / {2} ms   {3} KB   QP {4}   {5} fps" -f `
        $stats.encodeMean, $stats.encodeP95, $stats.encodeP99, $stats.deltaKB, $stats.avgQp, $stats.captureFps)
}

$out = Join-Path $ResultsDir 'native-bench.csv'
$results | Export-Csv -Path $out -NoTypeInformation -Encoding UTF8
Write-Host ''
Write-Host "matrix written to $out"
$results | Format-Table spec, encodeMean, encodeP95, encodeP99, deltaKB, avgQp, captureFps -AutoSize
