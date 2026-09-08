# ============================================================================
# The bench clip lives OUTSIDE the repository.
#
# cod.webm is ~260 MB of Call of Duty gameplay at 1440p60. Committing it would
# put a quarter of a gigabyte of binary into every clone forever, so this script
# resolves it from a local cache and tells the caller the file:// URL to open.
#
#   .\fetch-content.ps1                 # report where the clip is, or is not
#   .\fetch-content.ps1 -From <path>    # adopt a copy you already have
#
# The cache is %USERPROFILE%\.mw-bench\content (~/.mw-bench/content elsewhere),
# shared by every campaign and every worktree.
#
# There is deliberately no download URL here: the clip is a YouTube capture
# (yaNr1hHAg2M, 1440p60 VP9, the 37-52 s stretch) kept for local benchmarking,
# and a script that re-downloads someone's video on demand is not something to
# commit. Point -From at the copy in a previous campaign's cache, or capture a
# fresh one; any 1440p60 gameplay clip works, as long as the SAME file is used
# for every pass being compared.
# ============================================================================
param(
    [string] $From,
    [string] $CacheDir = (Join-Path $env:USERPROFILE '.mw-bench\content'),
    [string] $Name = 'cod.webm'
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $CacheDir | Out-Null
$target = Join-Path $CacheDir $Name

if ($From) {
    if (-not (Test-Path $From)) { throw "no file at $From" }
    Copy-Item -Path $From -Destination $target -Force
    Write-Host "copied into the cache: $target"
}

if (-not (Test-Path $target)) {
    Write-Warning @"
No bench clip in the cache.

    expected: $target

Put a 1440p60 gameplay clip there (any file, as long as every pass being
compared uses the same one), or adopt one you already have:

    .\fetch-content.ps1 -From D:\somewhere\cod.webm

Until then the campaign can still run against the static and scrolling
contents, but the headline numbers will not be comparable to previous
campaigns, which all used this clip.
"@
    exit 2
}

$file = Get-Item $target
$hash = (Get-FileHash -Path $target -Algorithm SHA256).Hash
# Recorded, not enforced: nobody has a canonical hash for a personal capture,
# but every campaign report carries the one it ran against, so two campaigns
# that disagree can be told apart from two campaigns that used different clips.
[pscustomobject]@{
    path    = $target
    sizeMB  = [math]::Round($file.Length / 1MB, 1)
    sha256  = $hash
    url     = 'file:///' + ($target -replace '\\', '/')
} | Format-List

$page = Join-Path $PSScriptRoot 'content\cod.html'
$src = 'file:///' + ($target -replace '\\', '/')
Write-Host "open this in the content kiosk:"
Write-Host ("  file:///" + (($page -replace '\\', '/')) + "?src=" + [uri]::EscapeDataString($src))
