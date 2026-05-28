# read_env_var.ps1 — reads a named environment variable and outputs it
# as a single-line C string literal (with \n for newlines), ready for
# qmake DEFINES.  Used by backend.pro at qmake time for GitHub Actions.
param([Parameter(Mandatory=$true)] [string] $VarName)

$ErrorActionPreference = "Stop"

$val = [Environment]::GetEnvironmentVariable($VarName)
if (-not $val) { exit 0 }

# Escape for C string literal: backslash → \\, newlines → \n
$val = $val -replace '\\', '\\\\' -replace "`r`n", "\\n" -replace "`n", "\\n"

Write-Output $val
