# Moonlight-Web — Logo Generation Script
# Generates all derived PNGs and favicon.ico from the source logo.
# Requires ImageMagick (magick.exe) in PATH or at a known install location.

param(
    [string]$SourceLogo = "frontend/assets/logo.png"
)

$ProjectRoot = Split-Path -Parent $PSCommandPath
$SourceLogo = Join-Path $ProjectRoot $SourceLogo

# ── Find ImageMagick ─────────────────────────────────────────────────────────
$MagickPaths = @(
    "magick.exe"                          # In PATH
    "$env:ProgramFiles\ImageMagick-7.1.1-Q16-HDRI\magick.exe"
    "$env:ProgramFiles\ImageMagick-7.0.10-Q16\magick.exe"
    "${env:ProgramFiles(x86)}\ImageMagick-7.1.1-Q16-HDRI\magick.exe"
    "${env:ProgramFiles(x86)}\ImageMagick-7.0.10-Q16\magick.exe"
    "$env:LOCALAPPDATA\ImageMagick\magick.exe"
)

$Magick = $null
foreach ($p in $MagickPaths) {
    $resolved = Get-Command $p -ErrorAction SilentlyContinue
    if ($resolved) { $Magick = $resolved.Source; break }
}

if (-not $Magick) {
    Write-Error "ImageMagick (magick.exe) not found. Install from https://imagemagick.org"
    exit 1
}

Write-Host "Using ImageMagick: $Magick" -ForegroundColor Green

# ── Check source logo exists ────────────────────────────────────────────────
if (-not (Test-Path $SourceLogo)) {
    Write-Error "Source logo not found: $SourceLogo"
    exit 1
}

# ── Get source dimensions ───────────────────────────────────────────────────
$identify = & $Magick identify -format "%wx%h" $SourceLogo 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to identify source logo: $identify"
    exit 1
}
Write-Host "Source logo dimensions: $identify" -ForegroundColor Cyan

$parts = $identify -split 'x'
$srcW = [int]$parts[0]
$srcH = [int]$parts[1]

# ── Output directories ──────────────────────────────────────────────────────
$AssetsDir = Join-Path $ProjectRoot "frontend\assets"

# ── Standard sizes to generate ──────────────────────────────────────────────
$Sizes = @(
    @{ Size = 16;  Name = "favicon-16x16.png" }
    @{ Size = 32;  Name = "favicon-32x32.png" }
    @{ Size = 48;  Name = "favicon-48x48.png" }
    @{ Size = 72;  Name = "icon-72x72.png" }
    @{ Size = 96;  Name = "icon-96x96.png" }
    @{ Size = 128; Name = "icon-128x128.png" }
    @{ Size = 144; Name = "icon-144x144.png" }
    @{ Size = 152; Name = "icon-152x152.png" }
    @{ Size = 192; Name = "icon-192x192.png" }
    @{ Size = 256; Name = "icon-256x256.png" }
    @{ Size = 384; Name = "icon-384x384.png" }
    @{ Size = 512; Name = "icon-512x512.png" }
)

Write-Host "`n=== Generating PNG sizes ===" -ForegroundColor Yellow

foreach ($s in $Sizes) {
    $outFile = Join-Path $AssetsDir $s.Name
    Write-Host "  Generating $($s.Size)x$($s.Size) -> $($s.Name) ... " -NoNewline
    & $Magick convert $SourceLogo -resize "$($s.Size)x$($s.Size)" $outFile 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "OK" -ForegroundColor Green
    } else {
        Write-Host "FAILED" -ForegroundColor Red
    }
}

# ── Generate multi-resolution favicon.ico ───────────────────────────────────
Write-Host "`n=== Generating favicon.ico ===" -ForegroundColor Yellow
$FaviconIco = Join-Path $AssetsDir "favicon.ico"

# Create a temporary directory for the favicon frames
$TmpDir = Join-Path $ProjectRoot "tmp_favicon_frames"
New-Item -ItemType Directory -Path $TmpDir -Force | Out-Null

# Generate frames for 16x16, 32x32, 48x48
foreach ($size in @(16, 32, 48)) {
    $frame = Join-Path $TmpDir "frame_${size}.png"
    & $Magick convert $SourceLogo -resize "${size}x${size}" $frame 2>&1 | Out-Null
}

# Combine into .ico
& $Magick convert (Join-Path $TmpDir "frame_16.png") `
                  (Join-Path $TmpDir "frame_32.png") `
                  (Join-Path $TmpDir "frame_48.png") `
                  $FaviconIco 2>&1 | Out-Null

if ($LASTEXITCODE -eq 0) {
    Write-Host "  favicon.ico generated OK" -ForegroundColor Green
} else {
    Write-Host "  favicon.ico generation FAILED" -ForegroundColor Red
}

# Cleanup temp frames
Remove-Item -Path $TmpDir -Recurse -Force -ErrorAction SilentlyContinue

# ── Summary ─────────────────────────────────────────────────────────────────
Write-Host "`n=== Summary ===" -ForegroundColor Yellow
Write-Host "Source:     $SourceLogo ($identify)" -ForegroundColor Cyan
Write-Host "Output dir: $AssetsDir" -ForegroundColor Cyan
Write-Host ""

foreach ($s in $Sizes) {
    $path = Join-Path $AssetsDir $s.Name
    if (Test-Path $path) {
        $fi = Get-Item $path
        $sizeKB = [math]::Round($fi.Length / 1KB, 1)
        Write-Host "  [OK] $($s.Name) ($sizeKB KB)" -ForegroundColor Green
    } else {
        Write-Host "  [MISSING] $($s.Name)" -ForegroundColor Red
    }
}

$icoPath = $FaviconIco
if (Test-Path $icoPath) {
    $fi = Get-Item $icoPath
    $sizeKB = [math]::Round($fi.Length / 1KB, 1)
    Write-Host "  [OK] favicon.ico ($sizeKB KB)" -ForegroundColor Green
} else {
    Write-Host "  [MISSING] favicon.ico" -ForegroundColor Red
}

Write-Host "`nDone." -ForegroundColor Green
