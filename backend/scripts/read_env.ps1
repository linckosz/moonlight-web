# read_env.ps1 — reads the MW_CERT_KEY value from a .env file and outputs
# it as a single-line C string literal (with \n for newlines), ready for
# qmake DEFINES.  Used by backend.pro at qmake time.  No files generated.
param([Parameter(Mandatory=$true)] [string] $EnvPath)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $EnvPath)) {
    Write-Error "File not found: $EnvPath"
    exit 1
}

$content = Get-Content $EnvPath -Raw

# Find MW_CERT_KEY=... value (matches until end of file)
if ($content -notmatch 'MW_CERT_KEY=(.+)$') {
    exit 0
}

$key = $matches[1].Trim()

# Escape for C string literal in DEFINES: backslash → \\, newlines → \n
$key = $key -replace '\\', '\\\\' -replace "`r`n", "\\n" -replace "`n", "\\n"

Write-Output $key
