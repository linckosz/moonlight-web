<#
.SYNOPSIS
    Downloads the latest zrok binary for all 3 platforms.
    Run this from the backend/tools/ directory.

.DESCRIPTION
    Fetches the latest release from openziti/zrok GitHub, downloads and
    extracts the zrok binary into platform-specific subdirectories:

        windows/zrok.exe
        linux/zrok
        macos/zrok

    Usage:
        powershell -ExecutionPolicy Bypass -File download_binaries.ps1

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

    # 2. Define targets
    $targets = @(
        @{
            Name = "Windows"
            SubDir = "windows"
            ExeName = "zrok.exe"
            Patterns = @("windows.*amd64", "windows.*x86_64")
            ArchiveType = "zip"
        },
        @{
            Name = "Linux"
            SubDir = "linux"
            ExeName = "zrok"
            Patterns = @("linux.*amd64", "linux.*x86_64")
            ArchiveType = "tar.gz"
        },
        @{
            Name = "macOS"
            SubDir = "macos"
            ExeName = "zrok"
            Patterns = @("darwin.*amd64", "darwin.*x86_64", "darwin.*arm64")
            ArchiveType = "tar.gz"
        }
    )

    foreach ($target in $targets) {
        Write-Host "`n[zrok] Processing $($target.Name)..." -ForegroundColor Cyan

        # Find the asset
        $asset = $release.assets | Where-Object {
            $matched = $false
            foreach ($pattern in $target.Patterns) {
                if ($_.name -match $pattern) { $matched = $true; break }
            }
            $matched
        } | Select-Object -First 1

        if (-not $asset) {
            Write-Host "[zrok] WARNING: No asset found for $($target.Name) with patterns: $($target.Patterns -join ', ')" -ForegroundColor Yellow
            Write-Host "  Available assets for this target:"
            $release.assets | ForEach-Object { Write-Host "  - $($_.name)" }
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
        Write-Host "[zrok]   Downloading: $downloadUrl"
        Invoke-WebRequest -Uri $downloadUrl -OutFile $archivePath -UserAgent "moonlight-web-download"

        # Extract
        Write-Host "[zrok]   Extracting..."
        if ($archiveName -match "\.zip$") {
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            $zip = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
            $entry = $zip.Entries | Where-Object { $_.Name -eq $target.ExeName } | Select-Object -First 1
            if (-not $entry) {
                Write-Host "[zrok]   ERROR: $($target.ExeName) not found in ZIP archive" -ForegroundColor Red
                Write-Host "  Contents:"
                $zip.Entries | ForEach-Object { Write-Host "    - $($_.Name)" }
                $zip.Dispose()
                Remove-Item -Path $archivePath -Force -ErrorAction SilentlyContinue
                continue
            }
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $targetExe, $true)
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
                } else {
                    Write-Host "[zrok]   ERROR: $($target.ExeName) not found in tar.gz" -ForegroundColor Red
                    Remove-Item -Path $archivePath -Force -ErrorAction SilentlyContinue
                    continue
                }
            }
            finally {
                Remove-Item -Path $extractDir -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
        else {
            Write-Host "[zrok]   ERROR: Unknown archive format: $archiveName" -ForegroundColor Red
            Remove-Item -Path $archivePath -Force -ErrorAction SilentlyContinue
            continue
        }

        # Clean up archive
        Remove-Item -Path $archivePath -Force -ErrorAction SilentlyContinue

        # Verify
        if (-not (Test-Path $targetExe)) {
            Write-Host "[zrok]   ERROR: Binary not found at $targetExe after extraction" -ForegroundColor Red
            continue
        }

        $fileInfo = Get-Item $targetExe
        Write-Host "[zrok]   SUCCESS: $targetExe ($([math]::Round($fileInfo.Length / 1KB)) KB)" -ForegroundColor Green
    }

    # Remove .gitkeep files from target dirs (they were placeholders)
    Get-ChildItem -Path $ScriptDir -Recurse -Filter ".gitkeep" | Remove-Item -Force -ErrorAction SilentlyContinue

    Write-Host "`n[zrok] All done!" -ForegroundColor Cyan
}
catch {
    Write-Host "[zrok] ERROR: $_" -ForegroundColor Red
    exit 1
}
