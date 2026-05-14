<#
.SYNOPSIS
    Download Node.js runtime and install nport package into runtime/
.DESCRIPTION
    Ensures Node.js v26.1.0 is available in runtime/node/ and the nport
    package is installed in runtime/nport/node_modules/.

    Platforms:
      - Windows x64: runtime/node/node.exe
      - Future: Linux / macOS
#>

$ErrorActionPreference = "Stop"

# Determine the script's directory (backend/tools/)
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
# Project root (two levels up from backend/tools/)
$ProjectRoot = Resolve-Path (Join-Path $ScriptDir "..\..")

# Node.js version to install
$NodeVersion = "26.1.0"

# Platform detection
$IsWindows = [Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT

function Ensure-NodeRuntime {
    $NodeDir = Join-Path $ProjectRoot "runtime\node"
    $NodeExe = Join-Path $NodeDir "node.exe"

    if (Test-Path $NodeExe) {
        Write-Host "[prepare_node_nport] Node.js found at: $NodeExe"
        return $true
    }

    Write-Host "[prepare_node_nport] Node.js v$NodeVersion not found. Downloading..."

    # Create runtime directory
    if (-not (Test-Path $NodeDir)) {
        New-Item -ItemType Directory -Path $NodeDir -Force | Out-Null
    }

    if ($IsWindows) {
        $ArchiveName = "node-v$NodeVersion-win-x64.zip"
        $DownloadUrl = "https://nodejs.org/dist/v$NodeVersion/$ArchiveName"
        $ArchivePath = Join-Path $NodeDir "node.zip"

        Write-Host "[prepare_node_nport] Downloading $DownloadUrl ..."
        try {
            Invoke-WebRequest -Uri $DownloadUrl -OutFile $ArchivePath -UseBasicParsing
        } catch {
            Write-Error "[prepare_node_nport] Failed to download Node.js: $_"
            exit 1
        }

        Write-Host "[prepare_node_nport] Extracting $ArchivePath ..."
        try {
            Expand-Archive -Path $ArchivePath -DestinationPath $NodeDir -Force
        } catch {
            Write-Error "[prepare_node_nport] Failed to extract Node.js archive: $_"
            exit 1
        }

        # Expand-Archive creates node-v26.1.0-win-x64/ subdirectory; move contents up
        $ExtractedDir = Join-Path $NodeDir "node-v$NodeVersion-win-x64"
        if (Test-Path $ExtractedDir) {
            Write-Host "[prepare_node_nport] Moving files from $ExtractedDir to $NodeDir ..."
            Get-ChildItem -Path $ExtractedDir | Move-Item -Destination $NodeDir -Force
            Remove-Item -Path $ExtractedDir -Force -Recurse
        }

        Remove-Item -Path $ArchivePath -Force

        Write-Host "[prepare_node_nport] Cleaned up archive"
    } else {
        # macOS / Linux — not yet tested but prepare the structure
        $IsMacOS = [Environment]::OSVersion.Platform -eq [PlatformID]::MacOSX
        if ($IsMacOS) {
            $ArchiveName = "node-v$NodeVersion-darwin-x64.tar.gz"
        } else {
            # Detect ARM vs x64
            $Arch = if ([Environment]::Is64BitOperatingSystem) { "x64" } else { "arm64" }
            $ArchiveName = "node-v$NodeVersion-linux-$Arch.tar.gz"
        }
        $DownloadUrl = "https://nodejs.org/dist/v$NodeVersion/$ArchiveName"
        $ArchivePath = Join-Path $NodeDir "node.tar.gz"

        Write-Host "[prepare_node_nport] Downloading $DownloadUrl ..."
        try {
            Invoke-WebRequest -Uri $DownloadUrl -OutFile $ArchivePath -UseBasicParsing
        } catch {
            Write-Error "[prepare_node_nport] Failed to download Node.js: $_"
            exit 1
        }

        Write-Host "[prepare_node_nport] Extracting $ArchivePath ..."
        try {
            tar -xzf $ArchivePath -C $NodeDir
        } catch {
            Write-Error "[prepare_node_nport] Failed to extract Node.js archive: $_"
            exit 1
        }

        # tar extracts to node-v<version>-<platform>-<arch>/; move contents up
        $ExtractedDir = Join-Path $NodeDir "node-v$NodeVersion*"
        $Dirs = Get-ChildItem -Path $ExtractedDir -Directory
        if ($Dirs.Count -gt 0) {
            Get-ChildItem -Path $Dirs[0].FullName | Move-Item -Destination $NodeDir -Force
            Remove-Item -Path $Dirs[0].FullName -Force -Recurse
        }

        Remove-Item -Path $ArchivePath -Force
        Write-Host "[prepare_node_nport] Cleaned up archive"
    }

    if ((Test-Path $NodeExe) -or (-not $IsWindows -and (Test-Path (Join-Path $NodeDir "bin\node")))) {
        Write-Host "[prepare_node_nport] Node.js v$NodeVersion installed at: $NodeDir"
        return $true
    } else {
        Write-Error "[prepare_node_nport] Node.js binary not found after extraction"
        exit 1
    }
}

function Ensure-NportPackage {
    $NportDir = Join-Path $ProjectRoot "runtime\nport"
    $NportModules = Join-Path $NportDir "node_modules\nport"

    if (Test-Path $NportModules) {
        Write-Host "[prepare_node_nport] nport package already installed at: $NportModules"
        return $true
    }

    Write-Host "[prepare_node_nport] Installing nport package..."

    # Locate npm
    $NodeDir = Join-Path $ProjectRoot "runtime\node"
    $NpmCmd = Join-Path $NodeDir "npm.cmd"

    if (-not (Test-Path $NpmCmd)) {
        # On non-Windows, npm is at bin/npm
        $NpmCmd = Join-Path $NodeDir "bin\npm"
    }

    if (-not (Test-Path $NpmCmd)) {
        Write-Error "[prepare_node_nport] npm not found at $NpmCmd"
        exit 1
    }

    # Create nport directory with package.json if not exists
    if (-not (Test-Path (Join-Path $NportDir "package.json"))) {
        if (-not (Test-Path $NportDir)) {
            New-Item -ItemType Directory -Path $NportDir -Force | Out-Null
        }
        @'
{
    "name": "nport-wrapper",
    "private": true,
    "dependencies": {
        "nport": "latest"
    }
}
'@ | Set-Content -Path (Join-Path $NportDir "package.json")
    }

    # Run npm install
    Push-Location $NportDir
    try {
        Write-Host "[prepare_node_nport] Running: & $NpmCmd install ..."
        & $NpmCmd install
        if ($LASTEXITCODE -ne 0) {
            Write-Error "[prepare_node_nport] npm install failed with exit code $LASTEXITCODE"
            exit 1
        }
    } finally {
        Pop-Location
    }

    if (Test-Path $NportModules) {
        Write-Host "[prepare_node_nport] nport installed successfully"
        return $true
    } else {
        Write-Error "[prepare_node_nport] nport package not found after install"
        exit 1
    }
}

# ── Main ──────────────────────────────────────────────────────────────────────

Write-Host "[prepare_node_nport] === Starting Node.js & nport setup ==="

Ensure-NodeRuntime
Ensure-NportPackage

Write-Host "[prepare_node_nport] === Setup complete ==="
