# Moonlight-Web — backend coverage gate.
#
# Parses an OpenCppCoverage Cobertura XML and enforces a minimum LINE coverage
# over the in-scope production sources only (the TNR target set). Exits non-zero
# when the aggregate is below the threshold so it can gate CI / PR validation.
#
# Usage:
#   powershell -File check_coverage.ps1 -CoverageXml cov.xml [-Threshold 70]

param(
    [Parameter(Mandatory = $true)][string]$CoverageXml,
    [int]$Threshold = 70,
    [string[]]$Files = @(
        'InputEncoder.cpp',
        'StreamConfig.cpp',
        'InputCrypto.cpp',
        'RestRouter.cpp',
        'AppSettings.cpp',
        'AuthManager.cpp'
    )
)

if (-not (Test-Path $CoverageXml)) {
    Write-Error "Coverage file not found: $CoverageXml"
    exit 2
}

[xml]$doc = Get-Content -Raw -Path $CoverageXml

# Collect every <class> node (Cobertura nests them under packages/package/classes).
$classes = $doc.SelectNodes('//class')

$totalLines = 0
$coveredLines = 0
$rows = @()

foreach ($leaf in $Files) {
    $fileTotal = 0
    $fileCovered = 0
    foreach ($cls in $classes) {
        $fn = [string]$cls.filename
        if ([string]::IsNullOrEmpty($fn)) { continue }
        if (-not ($fn.Replace('\','/').ToLower().EndsWith('/' + $leaf.ToLower())) -and
            -not ([System.IO.Path]::GetFileName($fn).ToLower() -eq $leaf.ToLower())) { continue }
        foreach ($line in $cls.SelectNodes('.//line')) {
            $fileTotal++
            if ([int]$line.hits -gt 0) { $fileCovered++ }
        }
    }
    $pct = if ($fileTotal -gt 0) { [math]::Round(100.0 * $fileCovered / $fileTotal, 1) } else { 0 }
    $rows += [pscustomobject]@{ File = $leaf; Covered = $fileCovered; Total = $fileTotal; Percent = $pct }
    $totalLines += $fileTotal
    $coveredLines += $fileCovered
}

$aggregate = if ($totalLines -gt 0) { [math]::Round(100.0 * $coveredLines / $totalLines, 1) } else { 0 }

Write-Host ''
Write-Host '==== Backend coverage (in-scope sources) ===='
$rows | Format-Table -AutoSize | Out-String | Write-Host
Write-Host ("Aggregate: {0}% ({1}/{2} lines)  threshold {3}%" -f $aggregate, $coveredLines, $totalLines, $Threshold)

if ($totalLines -eq 0) {
    Write-Error 'No coverage data matched the in-scope files — check OpenCppCoverage --sources filter.'
    exit 2
}

if ($aggregate -lt $Threshold) {
    Write-Host ("[FAIL] Coverage {0}% is below the {1}% gate." -f $aggregate, $Threshold) -ForegroundColor Red
    exit 1
}

Write-Host ("[OK] Coverage {0}% meets the {1}% gate." -f $aggregate, $Threshold) -ForegroundColor Green
exit 0
