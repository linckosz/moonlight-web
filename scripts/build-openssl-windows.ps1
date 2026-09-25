#requires -version 5
<#
.SYNOPSIS
    Builds the OpenSSL DLLs MoonlightWeb ships on Windows, and with -Install
    puts them where backend/CMakeLists.txt picks them up.

.DESCRIPTION
    The Windows packages do not build OpenSSL: they link against the import
    libraries and ship the DLLs committed under backend/libs/windows, one folder
    per architecture (lib/<arch>, include/<arch>). This script is how those
    files are made, so that a refresh is one command and the result says what
    produced it.

    Why not let the CI build OpenSSL on each run: in September 2026 the
    windows-11-arm runner image moved to MSVC 14.51, and the same OpenSSL 3.6.4
    source it compiled crashed on the first TLS handshake. A library that
    changes under a fixed version number, with nobody looking, is exactly what
    the committed prebuilt is meant to prevent. See docs/design/openssl-windows.md.

    Everything that decides the output is pinned here (OpenSSL version and
    source hash, Perl, NASM) or recorded next to the DLLs (compiler, assembler,
    Configure line, SHA-256 of every file) in lib/<arch>/BUILD-INFO.txt.

    What it needs on the machine: Visual Studio or its Build Tools with the C++
    toolset for the target (the "ARM64 build tools" component for -Arch arm64),
    and for ARM64 assembly a clang-cl (Visual Studio's "C++ Clang tools"
    component, an LLVM install, or -ClangCl). Perl and NASM are downloaded into
    the work folder when not given.

.EXAMPLE
    scripts\build-openssl-windows.ps1 -Arch arm64 -Install
.EXAMPLE
    # A variant for a bisection, left in the work folder, nothing installed:
    scripts\build-openssl-windows.ps1 -Arch arm64 -Variant no-asm -Tag noasm
#>
param(
    [ValidateSet('x64', 'arm64')]
    [string]$Arch = 'arm64',
    [string]$Version = '3.6.4',
    # asm: the target's assembly (NASM on x64, clang-cl on ARM64); no-asm: C only.
    [ValidateSet('asm', 'no-asm')]
    [string]$Variant = 'asm',
    # Extra compiler flags, appended after OpenSSL's own (e.g. '-O1').
    [string[]]$ExtraFlags = @(),
    # Distinguishes the build folder of one experiment from another.
    [string]$Tag = '',
    [string]$WorkDir = (Join-Path $env:LOCALAPPDATA 'mw-openssl'),
    [string]$VsPath = '',
    [string]$Perl = '',
    [string]$Nasm = '',
    [string]$ClangCl = '',
    [string]$Jom = '',
    # Copy the result into backend/libs/windows (DLLs, import libs, headers, BUILD-INFO.txt).
    [switch]$Install
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path $PSScriptRoot -Parent

# Official release tarballs (github.com/openssl/openssl/releases), SHA-256 from
# the .sha256 file published beside each one.
$OpenSslSha256 = @{
    '3.6.4' = '9bffaa1ad1e07b354c21bd3324ec02fa15579f45a7d0494b3e74bc449b7333ef'
}
$PerlZip = @{
    Url    = 'https://github.com/StrawberryPerl/Perl-Dist-Strawberry/releases/download/SP_54221_64bit/strawberry-perl-5.42.2.1-64bit-portable.zip'
    Sha256 = '32d83be90cf04b807cfb9477482bc36302cdee6f5b04cf57e81adecbd8f07898'
}
$NasmZip = @{
    Url    = 'https://www.nasm.us/pub/nasm/releasebuilds/3.01/win64/nasm-3.01-win64.zip'
    Sha256 = 'e0ba5157007abc7b1a65118a96657a961ddf55f7e3f632ee035366dfce039ca4'
}

function Step([string]$msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

function Get-Pinned([hashtable]$what, [string]$dest) {
    if (-not (Test-Path $dest)) {
        Step "Downloading $($what.Url)"
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $what.Url -OutFile "$dest.part" -UseBasicParsing
        Move-Item "$dest.part" $dest -Force
    }
    $h = (Get-FileHash $dest -Algorithm SHA256).Hash.ToLower()
    if ($h -ne $what.Sha256) {
        Remove-Item $dest -Force
        throw "SHA-256 mismatch for $dest (got $h, pinned $($what.Sha256)) - file removed"
    }
    return $dest
}

function Expand-Once([string]$zip, [string]$dest) {
    if (-not (Test-Path $dest)) {
        # Windows' bsdtar reads zip files, in seconds where Expand-Archive takes
        # minutes on Strawberry Perl's tens of thousands of files.
        New-Item -ItemType Directory -Force -Path "$dest.part" | Out-Null
        & "$env:SystemRoot\System32\tar.exe" -xf $zip -C "$dest.part"
        if ($LASTEXITCODE -ne 0) { throw "tar could not extract $zip" }
        Move-Item "$dest.part" $dest
    }
}

# Imports the environment a vcvarsall.bat call leaves behind.
function Import-VcVars([string]$vcvarsall, [string]$arg) {
    # stderr too: vcvarsall complains when vswhere is not on PATH, harmlessly,
    # and PowerShell 5.1 would stop on it.
    $dump = & cmd.exe /c "call `"$vcvarsall`" $arg >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) { throw "vcvarsall $arg failed" }
    foreach ($line in $dump) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2]) }
    }
}

if (-not $OpenSslSha256.ContainsKey($Version)) {
    throw "No pinned SHA-256 for OpenSSL ${Version}: add it to `$OpenSslSha256 (from the release's .sha256 file)"
}
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

# ── Sources and tools ────────────────────────────────────────────────────
$tarball = Get-Pinned @{
    Url    = "https://github.com/openssl/openssl/releases/download/openssl-$Version/openssl-$Version.tar.gz"
    Sha256 = $OpenSslSha256[$Version]
} (Join-Path $WorkDir "openssl-$Version.tar.gz")
$src = Join-Path $WorkDir "openssl-$Version"
if (-not (Test-Path (Join-Path $src 'Configure'))) {
    Step "Extracting $tarball"
    & "$env:SystemRoot\System32\tar.exe" -xzf $tarball -C $WorkDir
    if ($LASTEXITCODE -ne 0) { throw 'tar failed' }
}

if (-not $Perl) {
    $zip = Get-Pinned $PerlZip (Join-Path $WorkDir 'strawberry-perl-portable.zip')
    Expand-Once $zip (Join-Path $WorkDir 'perl')
    $Perl = Join-Path $WorkDir 'perl\perl\bin\perl.exe'
}
$env:PATH = "$(Split-Path $Perl);$env:PATH"

if ($Arch -eq 'x64' -and $Variant -eq 'asm') {
    if (-not $Nasm) {
        $zip = Get-Pinned $NasmZip (Join-Path $WorkDir 'nasm.zip')
        Expand-Once $zip (Join-Path $WorkDir 'nasm')
        $Nasm = Join-Path $WorkDir 'nasm\nasm-3.01\nasm.exe'
    }
    $env:PATH = "$(Split-Path $Nasm);$env:PATH"
}

if (-not $VsPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $VsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $VsPath) { throw 'No Visual Studio with the C++ toolset found (pass -VsPath)' }
}
$vcvarsall = Join-Path $VsPath 'VC\Auxiliary\Build\vcvarsall.bat'
Import-VcVars $vcvarsall $(if ($Arch -eq 'arm64') { 'amd64_arm64' } else { 'amd64' })

if ($Arch -eq 'arm64' -and $Variant -eq 'asm') {
    if (-not $ClangCl) {
        $candidates = @((Join-Path $VsPath 'VC\Tools\Llvm\x64\bin\clang-cl.exe'),
                        (Join-Path $env:ProgramFiles 'LLVM\bin\clang-cl.exe'))
        $onPath = Get-Command clang-cl.exe -ErrorAction SilentlyContinue
        if ($onPath) { $candidates += $onPath.Source }
        $ClangCl = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
        if (-not $ClangCl) { throw 'ARM64 assembly needs clang-cl: install "C++ Clang tools" in Visual Studio, or pass -ClangCl (or use -Variant no-asm)' }
    }
    $env:PATH = "$(Split-Path $ClangCl);$env:PATH"
}

# ── Configure and build ──────────────────────────────────────────────────
$target = if ($Arch -eq 'x64') { 'VC-WIN64A' } elseif ($Variant -eq 'asm') { 'VC-WIN64-CLANGASM-ARM' } else { 'VC-WIN64-ARM' }
$suffix = if ($Tag) { "-$Tag" } else { '' }
$build = Join-Path $WorkDir "build-$Version-$Arch-$Variant$suffix"
if (Test-Path $build) { Remove-Item $build -Recurse -Force }
New-Item -ItemType Directory -Force -Path $build | Out-Null

# shared: libcrypto-3-<arch>.dll + libssl-3-<arch>.dll with their import libs,
# which is what the app links (OpenSSL's VC targets write a PDB beside each, so
# a crash in them can be read). The install paths stay OpenSSL's Windows
# defaults, under Program Files: OpenSSL reads its config from OPENSSLDIR,
# which therefore must not be a folder an unprivileged user can create.
#
# CFLAGS starts from OpenSSL's own release value (/W3 /wd4090 /nologo /O2);
# /FS lets parallel cl processes (jom) share those PDBs.
$cflags = @('/W3', '/wd4090', '/nologo', '/O2', '/FS')
# OpenSSL's VC targets also pass /Gs0: a stack probe (__chkstk) in every
# function. On ARM64, MSVC 14.51 places that call before LR is saved in some
# shrink-wrapped prologues, and those functions return into themselves —
# tls_parse_all_extensions among them, so every TLS handshake crashed. The
# compiler's default threshold (one 4 KB page, the last /Gs wins) probes only
# the frames that can step over the guard page, which is all a probe is for;
# the few left are checked below. See docs/design/openssl-windows.md.
if ($Arch -eq 'arm64') { $cflags += '/Gs4096' }
$cflags += $ExtraFlags
# OpenSSL links with a bare /debug, which also means incremental linking
# (every call through a jump thunk) and no dead-code or identical-code folding.
$ldflags = '/nologo /debug /INCREMENTAL:NO /OPT:REF /OPT:ICF'
$configureArgs = @($target, 'shared', 'no-docs', 'no-tests', 'no-apps',
                   "CFLAGS=$($cflags -join ' ')", "LDFLAGS=$ldflags")
if ($Variant -eq 'no-asm') { $configureArgs += 'no-asm' }

Push-Location $build
# Perl and the make tools write progress on stderr; once this script's output
# is redirected, PowerShell 5.1 would stop on the first line of it. Exit codes
# decide instead.
$ErrorActionPreference = 'Continue'
try {
    Step "Configure $($configureArgs -join ' ')"
    & $Perl (Join-Path $src 'Configure') @configureArgs
    if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }

    $make = 'nmake'
    if (-not $Jom) { $j = Get-Command jom.exe -ErrorAction SilentlyContinue; if ($j) { $Jom = $j.Source } }
    if ($Jom) { $make = $Jom }
    Step "Build with $make"
    if ($Jom) { & $Jom /NOLOGO /J $env:NUMBER_OF_PROCESSORS build_libs } else { & nmake /NOLOGO build_libs }
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
} finally {
    $ErrorActionPreference = 'Stop'
    Pop-Location
}

# ── Collect ──────────────────────────────────────────────────────────────
$out = Join-Path $build 'out'
New-Item -ItemType Directory -Force -Path (Join-Path $out 'include\openssl') | Out-Null
$dlls = @("libcrypto-3-$Arch.dll", "libssl-3-$Arch.dll")
foreach ($f in $dlls + @("libcrypto-3-$Arch.pdb", "libssl-3-$Arch.pdb", 'libcrypto.lib', 'libssl.lib')) {
    Copy-Item (Join-Path $build $f) $out
}
# Public headers: the source's, then the ones Configure generated (opensslv.h,
# configuration.h, ...), which win.
Copy-Item (Join-Path $src 'include\openssl\*.h') (Join-Path $out 'include\openssl')
Copy-Item (Join-Path $build 'include\openssl\*.h') (Join-Path $out 'include\openssl') -Force

$machine = if ($Arch -eq 'arm64') { 'AA64' } else { '8664' }
foreach ($d in $dlls) {
    $hdr = & dumpbin /nologo /headers (Join-Path $out $d) | Select-String 'machine \('
    if ("$hdr" -notmatch $machine) { throw "$d is not a $Arch image: $hdr" }
}
if ($Arch -eq 'arm64') {
    Step 'Checking for prologues that call __chkstk before saving LR'
    $built = @($dlls | ForEach-Object { Join-Path $out $_ })
    & (Join-Path $PSScriptRoot 'check-arm64-chkstk.ps1') @built
    if ($LASTEXITCODE -ne 0) { throw 'A function would return into its own prologue: this compiler cannot build OpenSSL as configured' }
}

# cl prints its banner on stderr; through cmd so PowerShell 5.1 does not turn it into an error.
$clBanner = (& cmd.exe /c 'cl.exe 2>&1' | Select-Object -First 1).Trim()
$asBanner = switch ($true) {
    ($Variant -eq 'no-asm') { 'none (no-asm)' }
    ($Arch -eq 'x64') { (& $Nasm -v).Trim() }
    default { (& $ClangCl --version | Select-Object -First 1).Trim() }
}
$cflags = (Select-String -Path (Join-Path $build 'makefile') -Pattern '^CFLAGS=(.*)$').Matches[0].Groups[1].Value.Trim()
$lines = @(
    "OpenSSL $Version for Windows $Arch, built by scripts/build-openssl-windows.ps1",
    "Source:     openssl-$Version.tar.gz  sha256 $($OpenSslSha256[$Version])",
    "Configure:  $($configureArgs -join ' ')",
    "CFLAGS:     $cflags",
    "LDFLAGS:    $((Select-String -Path (Join-Path $build 'makefile') -Pattern '^LDFLAGS=(.*)$').Matches[0].Groups[1].Value.Trim())",
    "Compiler:   $clBanner ($env:VCToolsVersion)",
    "Assembler:  $asBanner",
    "Built:      $((Get-Date).ToString('yyyy-MM-dd')) on $env:COMPUTERNAME",
    '',
    'SHA-256:'
)
foreach ($f in $dlls + @('libcrypto.lib', 'libssl.lib')) {
    $lines += "  $((Get-FileHash (Join-Path $out $f) -Algorithm SHA256).Hash.ToLower())  $f"
}
$lines | Set-Content -Path (Join-Path $out 'BUILD-INFO.txt') -Encoding ascii
Get-Content (Join-Path $out 'BUILD-INFO.txt')

if ($Install) {
    $lib = Join-Path $RepoRoot "backend\libs\windows\lib\$Arch"
    $inc = Join-Path $RepoRoot "backend\libs\windows\include\$Arch\openssl"
    Step "Installing into $lib and $inc"
    New-Item -ItemType Directory -Force -Path $lib | Out-Null
    if (Test-Path $inc) { Remove-Item $inc -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $inc | Out-Null
    foreach ($f in $dlls + @('libcrypto.lib', 'libssl.lib', 'BUILD-INFO.txt')) { Copy-Item (Join-Path $out $f) $lib -Force }
    Copy-Item (Join-Path $out 'include\openssl\*.h') $inc
}
Step "Done: $out"
exit 0
