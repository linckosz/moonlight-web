<#
.SYNOPSIS
    Downloads the latest zrok release for Windows, Linux, and macOS (amd64)
    and extracts each binary to backend/tools/{windows,linux,macos}/.

    Usage:
        powershell -ExecutionPolicy Bypass -File download_zrok.ps1

    Requirements:
        - PowerShell 5.1+
        - Internet access (HTTPS outbound to github.com)
        - tar.exe (built-in on Windows 10+)
#>

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host "[zrok] Fetching latest release info from GitHub..." -ForegroundColor Cyan

try {
    # 1. Get latest release from GitHub API
    $apiUrl = "https://api.github.com/repos/openziti/zrok/releases/latest"
    $release = Invoke-RestMethod -Uri $apiUrl -Headers @{
        "Accept" = "application/vnd.github.v3+json"
        "User-Agent" = "moonlight-web-download"
    }

    $tagName = $release.tag_name
    Write-Host "[zrok] Latest release: $tagName" -ForegroundColor Green

    # 2. Define targets (platform -> subdir -> exe name)
    $targets = @(
        @{
            Name = "Windows"
            SubDir = "windows"
            ExeName = "zrok.exe"
            Patterns = @("windows.*amd64", "windows.*x86_64")
        },
        @{
            Name = "Linux"
            SubDir = "linux"
            ExeName = "zrok"
            Patterns = @("linux.*amd64", "linux.*x86_64")
        },
        @{
            Name = "macOS"
            SubDir = "macos"
            ExeName = "zrok"
            Patterns = @("darwin.*amd64", "darwin.*x86_64")
        }
    )

    $globalSuccess = $true

    foreach ($target in $targets) {
        Write-Host "`n[zrok] ----- $($target.Name) -----" -ForegroundColor Cyan

        # Find the matching asset
        $asset = $release.assets | Where-Object {
            $matched = $false
            foreach ($pattern in $target.Patterns) {
                if ($_.name -match $pattern) { $matched = $true; break }
            }
            $matched
        } | Select-Object -First 1

        if (-not $asset) {
            Write-Host "[zrok] WARNING: No asset found for $($target.Name)" -ForegroundColor Yellow
            $globalSuccess = $false
            continue
        }

        $downloadUrl = $asset.browser_download_url
        $archiveName = $asset.name
        Write-Host "[zrok]   Asset: $archiveName"

        # Prepare target directory
        $targetDir = Join-Path $ScriptDir $target.SubDir
        if (-not (Test-Path $targetDir)) {
            New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
        }
        $targetExe = Join-Path $targetDir $target.ExeName

        # Download
        $archivePath = Join-Path $env:TEMP $archiveName
        Write-Host "[zrok]   Downloading..."
        Write-Host "[zrok]   From: $downloadUrl"
        Invoke-WebRequest -Uri $downloadUrl -OutFile $archivePath -UserAgent "moonlight-web-download"

        # Extract
        Write-Host "[zrok]   Extracting..."
        $extractOk = $false

        if ($archiveName -match "\.zip$") {
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            $zip = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
            $entry = $zip.Entries | Where-Object { $_.Name -eq $target.ExeName } | Select-Object -First 1
            if ($entry) {
                [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $targetExe, $true)
                $extractOk = $true
            } else {
                Write-Host "[zrok]   ERROR: $($target.ExeName) not found in ZIP" -ForegroundColor Red
            }
            $zip.Dispose()
        }
        elseif ($archiveName -match "\.tar\.gz$|\.tgz$") {
            $extractDir = Join-Path $env:TEMP "zrok_extract_$([System.IO.Path]::GetRandomFileName())"
            New-Item -ItemType Directory -Path $extractDir -Force | Out-Null
            try {
                & "tar.exe" -xzf $archivePath -C $extractDir
                $foundExe = Get-ChildItem -Path $extractDir -Recurse -Filter $target.ExeName | Select-Object -First 1
                if ($foundExe) {
                    Copy-Item -Path $foundExe.FullName -Destination $targetExe -Force
                    $extractOk = $true
                } else {
                    Write-Host "[zrok]   ERROR: $($target.ExeName) not found in tar.gz" -ForegroundColor Red
                }
            }
            finally {
                Remove-Item -Path $extractDir -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
        else {
            Write-Host "[zrok]   ERROR: Unknown archive format: $archiveName" -ForegroundColor Red
        }

        # Clean up archive
        Remove-Item -Path $archivePath -Force -ErrorAction SilentlyContinue

        if (-not $extractOk) {
            $globalSuccess = $false
            continue
        }

        # Verify
        if (-not (Test-Path $targetExe)) {
            Write-Host "[zrok]   ERROR: Binary not found after extraction" -ForegroundColor Red
            $globalSuccess = $false
            continue
        }

        $fileInfo = Get-Item $targetExe
        Write-Host "[zrok]   SUCCESS: $targetExe ($([math]::Round($fileInfo.Length / 1KB)) KB)" -ForegroundColor Green

        # Quick version check
        try {
            $version = & $targetExe --version 2>&1
            Write-Host "[zrok]   Version: $version" -ForegroundColor Green
        } catch {
            Write-Host "[zrok]   WARNING: Version check failed: $_" -ForegroundColor Yellow
        }
    }

    # Remove .gitkeep placeholders in target dirs
    Get-ChildItem -Path $ScriptDir -Recurse -Filter ".gitkeep" | Remove-Item -Force -ErrorAction SilentlyContinue

    Write-Host "`n[zrok] ========================================" -ForegroundColor Cyan
    if ($globalSuccess) {
        Write-Host "[zrok] All binaries downloaded successfully!" -ForegroundColor Green
        Write-Host "[zrok]"
        Write-Host "[zrok]   tools/windows/zrok.exe"
        Write-Host "[zrok]   tools/linux/zrok"
        Write-Host "[zrok]   tools/macos/zrok"
    } else {
        Write-Host "[zrok] Some platforms had errors (see above)" -ForegroundColor Yellow
    }
    Write-Host "[zrok] ========================================" -ForegroundColor Cyan
}
catch {
    Write-Host "[zrok] ERROR: $_" -ForegroundColor Red
    exit 1
}
