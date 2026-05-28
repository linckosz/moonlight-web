# read_env.ps1 — reads a named variable from a .env file and outputs
# its value as a single-line C string literal (with \n for newlines),
# ready for qmake DEFINES.  Used by backend.pro at qmake time.
param(
    [Parameter(Mandatory=$true)] [string] $VarName,
    [Parameter(Mandatory=$true)] [string] $EnvPath
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $EnvPath)) {
    Write-Error "File not found: $EnvPath"
    exit 1
}

$content = Get-Content $EnvPath -Raw

# Find VARNAME=... value in the file content
$pattern = "$VarName=(.*?)(?:\r?\n\w+=|\r?\n\r?\n|\Z)"
if ($content -notmatch $pattern) {
    exit 0
}

$key = $matches[1].Trim()

# Escape for C string literal in DEFINES: backslash → \\, newlines → \n
$key = $key -replace '\\', '\\\\' -replace "`r`n", "\\n" -replace "`n", "\\n"

Write-Output $key
