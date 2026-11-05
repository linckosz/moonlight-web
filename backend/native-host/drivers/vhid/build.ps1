# MoonlightWeb — builds "MoonlightWeb Virtual HID", the HID passthrough's Windows driver.
# Copyright (C) 2026 Bruno Martin. GPLv3.
#
# WinUHid's UMDF2 driver over Windows' Virtual HID Framework (third_party/winuhid, MIT), under our
# own names, with the engine's descriptor validation inside (DescriptorCheck.cpp), and the host's own
# mouse and keyboard as its two exceptions (input/SystemMouse.cpp, input/SystemKeyboard.cpp).
#
# Nothing is installed on the machine: the WDK and the SDK come from NuGet (packages.config, restored
# into packages/), and the WDK tools are called one by one — tracewpp, cl, link, stampinf, Inf2Cat,
# signtool — instead of through the WDK's MSBuild toolset, which needs Visual Studio's WDK component.
# Needs MSVC (Build Tools) for cl and link; run it from any PowerShell, it loads vcvars itself.
#
#   .\build.ps1                 build and test-sign into build\package\
#   .\build.ps1 -Configuration Debug
#   .\build.ps1 -Sign Own [-KeyDir <dir>] [-TimestampUrl <url>]
#   .\build.ps1 -Sign Product -CertThumbprint <sha1> [-TimestampUrl <url>]
#
# Signing, test (the default): a self-signed certificate made once with openssl into build\testcert\
# (never committed). Installing it needs a machine in test mode (bcdedit /set testsigning on), with
# the certificate in its LocalMachine Root and TrustedPublisher stores; install.ps1 does that part.
#
# Signing, own (the product today, Bruno's choice of 11/10/2026: no paid certificate): MoonlightWeb's
# own self-signed code-signing certificate, timestamped. The installer puts it in Root and
# TrustedPublisher for the install alone and takes it out again; the driver, once installed, loads
# without test mode, Secure Boot on (bench of 11/10/2026). Its private key lives in -KeyDir, by
# default %USERPROFILE%\.moonlightweb\vhid-signing, made there on first use, never inside the
# repository: whoever holds it signs packages our installer would take. Keep a copy of that folder
# somewhere safe; losing it only means a new certificate, and new pins, for the next driver.
#
# Signing, product: a code-signing certificate from a public CA, already in the user's certificate
# store (or on its token), named by thumbprint, timestamped. A user-mode driver needs nothing more
# (no EV, no Microsoft attestation: the virtual display's driver is signed that way).
#
# Own and Product: the package then goes to backend\installer\drivers\vhid, and the hashes and
# thumbprint printed at the end go to src\backend\VirtualHidApply.h: the installer installs only
# files that match them.
#
# x64 only. Windows on ARM takes a driver package only signed by Microsoft (WHQL or attestation),
# UMDF included: an ARM64 build of 0.1.2.0 under our own certificate, in Root and TrustedPublisher,
# was refused with 0x800b0109, "Code Integrity is enforced" (mw-arm, 20/10/2026). ARM64 hosts keep
# SendInput, as they always had.

param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    # The INF's DriverVer, with the build's date. Raise it with every driver change: of two packages
    # for the node, Windows installs the newer DriverVer, and two builds of one day tie on the date.
    # 0.1.0.0 was the first (11/10/2026); 0.1.1.0 fixes the lost GET_NEXT_EVENT wakeup; 0.1.2.0
    # takes the host's own mouse (input/SystemMouse.h, 19/10/2026); 0.1.3.0 its own keyboard
    # (input/SystemKeyboard.h, 04/11/2026).
    [string]$Version = '0.1.3.0',
    [ValidateSet('Test', 'Own', 'Product')]
    [string]$Sign = 'Test',
    [string]$CertThumbprint = '',
    [string]$KeyDir = (Join-Path $env:USERPROFILE '.moonlightweb\vhid-signing'),
    [string]$TimestampUrl = 'http://timestamp.digicert.com'
)
if ($Sign -eq 'Product' -and $CertThumbprint -notmatch '^[0-9A-Fa-f]{40}$') {
    throw '-Sign Product needs -CertThumbprint: the SHA-1 thumbprint of the code-signing certificate'
}
if ($Sign -eq 'Own') {
    # The key never lands in a working tree, where one `git add` would publish it.
    $repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path.TrimEnd('\') + '\'
    $full = [IO.Path]::GetFullPath($KeyDir).TrimEnd('\') + '\'
    if ($full.StartsWith($repo, [StringComparison]::OrdinalIgnoreCase)) {
        throw "-KeyDir $KeyDir is inside the repository: keep the signing key outside it"
    }
}

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$kit = '10.0.28000.2526'
$kitDir = '10.0.28000.0'
$umdf = '2.15' # the inbox VHF UMDF source driver's version: Windows 10 2004 and up
$pkgs = Join-Path $here 'packages'
$sdk = Join-Path $pkgs "Microsoft.Windows.SDK.CPP.$kit\c"
$sdkX64 = Join-Path $pkgs "Microsoft.Windows.SDK.CPP.x64.$kit\c"
$wdk = Join-Path $pkgs "Microsoft.Windows.WDK.x64.$kit\c"
$winuhid = Resolve-Path (Join-Path $here '..\..\third_party\winuhid\WinUHid Driver')
$engineSrc = Resolve-Path (Join-Path $here '..\..\src')
$engineInclude = Resolve-Path (Join-Path $here '..\..\include')
$out = Join-Path $here 'build'
$obj = Join-Path $out "obj\$Configuration"
$package = Join-Path $out 'package'

function Step($what) { Write-Host "== $what" }
function Run($exe, [string[]]$arguments) {
    # Windows PowerShell turns a tool's stderr into errors; only its exit code counts here.
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $exe @arguments 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -notmatch '^[.+*]+$' } | Out-Host
    $code = $LASTEXITCODE
    $ErrorActionPreference = $saved
    if ($code -ne 0) { throw "$([IO.Path]::GetFileName($exe)) failed ($code)" }
}

# ── Toolchain ────────────────────────────────────────────────────────────────
if (-not (Test-Path $wdk)) {
    Step 'NuGet restore (WDK and SDK, about 1.5 GB once)'
    $nuget = Join-Path $here '.tools\nuget.exe'
    if (-not (Test-Path $nuget)) {
        New-Item -ItemType Directory -Force (Split-Path $nuget) | Out-Null
        Invoke-WebRequest https://dist.nuget.org/win-x86-commandline/latest/nuget.exe -OutFile $nuget
    }
    Run $nuget @('restore', (Join-Path $here 'packages.config'), '-PackagesDirectory', $pkgs, '-NonInteractive')
}

# cl and link from the newest MSVC, with vcvars' environment taken into this process.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'no MSVC found (Visual Studio Build Tools with the C++ workload)' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
# (vcvars looks for a vswhere.exe on PATH and complains on stderr when there is none: harmless.)
cmd /c "`"$vcvars`" >nul 2>nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($matches[1])" $matches[2] }
}
# Headers and libraries of the NuGet kit, not of whatever SDK the machine has.
$env:INCLUDE = @(
    (Join-Path $wdk "Include\wdf\umdf\$umdf"),
    (Join-Path $wdk "Include\$kitDir\um"),
    (Join-Path $wdk "Include\$kitDir\shared"),
    (Join-Path $sdk "Include\$kitDir\um"),
    (Join-Path $sdk "Include\$kitDir\shared"),
    (Join-Path $sdk "Include\$kitDir\ucrt"),
    ($env:INCLUDE -split ';' | Where-Object { $_ -match 'MSVC' })
) -join ';'
$env:LIB = @(
    (Join-Path $wdk "Lib\wdf\umdf\x64\$umdf"),
    (Join-Path $wdk "Lib\$kitDir\um\x64"),
    (Join-Path $sdkX64 'um\x64'),
    (Join-Path $sdkX64 'ucrt\x64'),
    ($env:LIB -split ';' | Where-Object { $_ -match 'MSVC' })
) -join ';'

New-Item -ItemType Directory -Force $obj, $package | Out-Null

# ── WPP: WinUHid.c includes the WinUHid.tmh tracewpp writes ─────────────────
Step 'tracewpp'
$sdkBin = Join-Path $sdk "bin\$kitDir"
Run (Join-Path $sdkBin 'x64\tracewpp.exe') @(
    "-cfgdir:$(Join-Path $sdkBin 'WppConfig\Rev1')",
    "-scan:$(Join-Path $winuhid 'Trace.h')",
    "-odir:$obj", '-um', '-dll',
    (Join-Path $winuhid 'WinUHid.c'))

# ── Compile and link ─────────────────────────────────────────────────────────
Step "cl ($Configuration)"
$common = @('/nologo', '/c', '/W4', '/WX-', '/Zi', '/guard:cf', '/GS', '/Gy', '/Zc:wchar_t',
    "/Fd$obj\\", "/Fo$obj\\", "/I$obj", "/I$winuhid", "/I$engineSrc", "/I$engineInclude",
    '/DUNICODE', '/D_UNICODE', '/D_WINDLL', '/D_WIN64', '/D_AMD64_', '/DAMD64',
    '/DUMDF_VERSION_MAJOR=2', "/DUMDF_VERSION_MINOR=$($umdf.Split('.')[1])", '/DMOONLIGHTWEB_VHID',
    # From UMDF 2.15, WPP_INIT_TRACING takes the driver object and the registry path, as the WDK's
    # toolset sets up. C4005: the WPP header includes ntstatus.h after windows.h, which redefines
    # the handful of STATUS_ codes winnt.h has, to the same values.
    '/DWPP_MACRO_USE_KM_VERSION_FOR_UM', '/wd4005', '/wd4324')
if ($Configuration -eq 'Release') { $common += @('/O2', '/MT', '/DNDEBUG') } else { $common += @('/Od', '/MTd', '/DDBG=1') }
Run cl.exe ($common + @('/TC', (Join-Path $winuhid 'WinUHid.c')))
Run cl.exe ($common + @('/TP', '/std:c++17', '/EHsc', (Join-Path $here 'DescriptorCheck.cpp'),
        (Join-Path $engineSrc 'input\HidDescriptor.cpp'), (Join-Path $engineSrc 'input\SystemMouse.cpp'),
        (Join-Path $engineSrc 'input\SystemKeyboard.cpp')))

Step 'link'
$dll = Join-Path $package 'MoonlightWebVHid.dll'
Run link.exe @('/nologo', '/DLL', '/DEBUG', '/guard:cf', '/DYNAMICBASE', '/NXCOMPAT', '/OPT:REF',
    '/OPT:ICF', '/CETCOMPAT', "/OUT:$dll", "/PDB:$obj\MoonlightWebVHid.pdb",
    "/IMPLIB:$obj\MoonlightWebVHid.lib", "$obj\WinUHid.obj", "$obj\DescriptorCheck.obj",
    "$obj\HidDescriptor.obj", "$obj\SystemMouse.obj", "$obj\SystemKeyboard.obj",
    'WdfDriverStubUm.lib', 'VhfUm.lib', 'ntdll.lib', 'kernel32.lib', 'advapi32.lib')

# ── INF and catalog ─────────────────────────────────────────────────────────
Step 'stampinf'
$inf = Join-Path $package 'MoonlightWebVHid.inf'
Copy-Item (Join-Path $here 'MoonlightWebVHid.inf') $inf -Force
Run (Join-Path $wdk "bin\$kitDir\x64\stampinf.exe") @('-f', $inf, '-d', '*', '-a', 'amd64',
    '-u', "$umdf.0", '-v', $Version)

Step 'Inf2Cat'
Remove-Item (Join-Path $package 'MoonlightWebVHid.cat') -ErrorAction SilentlyContinue
Run (Join-Path $wdk "bin\$kitDir\x86\Inf2Cat.exe") @("/driver:$package", '/os:10_X64', '/uselocaltime')

$signtool = Join-Path $sdkBin 'x64\signtool.exe'
$cat = Join-Path $package 'MoonlightWebVHid.cat'

# A self-signed code-signing certificate in $dir, made once with openssl: the .pfx (its password in
# pfx-password.txt beside it) and the .cer. A config of our own: openssl's default marks a
# self-signed certificate CA:TRUE, and Windows refuses a CA certificate as a driver's signer
# (TRUST_E_BASIC_CONSTRAINTS). Signing code is all it may do.
function Get-SigningCert($dir, $name, $cn, $days) {
    $c = @{
        Pfx  = Join-Path $dir "$name.pfx"
        Cer  = Join-Path $dir "$name.cer"
        Pass = Join-Path $dir 'pfx-password.txt'
    }
    if (-not (Test-Path $c.Pfx)) {
        Step "new certificate `"$cn`" in $dir"
        New-Item -ItemType Directory -Force $dir | Out-Null
        $openssl = (Get-Command openssl -ErrorAction SilentlyContinue).Source
        if (-not $openssl) { $openssl = 'C:\Program Files\Git\usr\bin\openssl.exe' }
        $pass = [Guid]::NewGuid().ToString('N')
        Set-Content -Path $c.Pass -Value $pass -Encoding ascii
        $key = Join-Path $dir 'key.pem'
        $pem = Join-Path $dir 'cert.pem'
        $cnf = Join-Path $dir 'openssl.cnf'
        Set-Content -Path $cnf -Encoding ascii -Value @(
            '[req]', 'distinguished_name = dn', 'x509_extensions = ext', 'prompt = no',
            '[dn]', "CN = $cn",
            '[ext]', 'basicConstraints = critical, CA:FALSE', 'keyUsage = critical, digitalSignature',
            'extendedKeyUsage = codeSigning', 'subjectKeyIdentifier = hash')
        Run $openssl @('req', '-x509', '-config', $cnf, '-newkey', 'rsa:3072', '-sha256', '-days', "$days",
            '-nodes', '-keyout', $key, '-out', $pem)
        Run $openssl @('pkcs12', '-export', '-inkey', $key, '-in', $pem, '-out', $c.Pfx, '-passout', "pass:$pass")
        Run $openssl @('x509', '-in', $pem, '-outform', 'der', '-out', $c.Cer)
        Remove-Item $key
    }
    $c.Password = (Get-Content $c.Pass -Raw).Trim()
    $c.Thumbprint = (New-Object Security.Cryptography.X509Certificates.X509Certificate2 $c.Cer).Thumbprint
    return $c
}

# What VirtualHidApply.h pins: the installer refuses anything else.
function Write-Pins($signer) {
    Step 'pins for src\backend\VirtualHidApply.h'
    foreach ($name in 'MoonlightWebVHid.inf', 'MoonlightWebVHid.cat', 'MoonlightWebVHid.dll') {
        $h = (Get-FileHash -Algorithm SHA256 (Join-Path $package $name)).Hash.ToLowerInvariant()
        Write-Host "    {`"$name`", `"$h`"},"
    }
    Write-Host "    catalog signer: $($signer.ToLowerInvariant())"
    Step "done: $package (copy it to backend\installer\drivers\vhid)"
    Get-ChildItem $package | Format-Table Name, Length -AutoSize
}

Remove-Item (Join-Path $package 'MoonlightWebVHid-test.cer') -ErrorAction SilentlyContinue

# ── Product signature, a public CA's certificate ────────────────────────────
if ($Sign -eq 'Product') {
    Step 'product signature'
    foreach ($f in @($dll, $cat)) {
        Run $signtool @('sign', '/q', '/sha1', $CertThumbprint, '/fd', 'sha256',
            '/tr', $TimestampUrl, '/td', 'sha256', $f)
        Run $signtool @('verify', '/q', '/pa', $f)
    }
    Write-Pins $CertThumbprint
    return
}

# ── Product signature, our own certificate ──────────────────────────────────
if ($Sign -eq 'Own') {
    Step 'own signature'
    # Twenty years: the installer checks it when the driver goes in, and the timestamp keeps
    # what was signed while it lived valid after it.
    $c = Get-SigningCert $KeyDir 'MoonlightWebVHid' 'MoonlightWeb Virtual HID' 7300
    foreach ($f in @($dll, $cat)) {
        Run $signtool @('sign', '/q', '/fd', 'sha256', '/f', $c.Pfx, '/p', $c.Password,
            '/tr', $TimestampUrl, '/td', 'sha256', $f)
        # signtool verify wants a trusted root, which this machine has no reason to make ours:
        # the signer and the timestamp are read instead, and the chain left to the installer.
        $s = Get-AuthenticodeSignature $f
        if (-not $s.SignerCertificate -or $s.SignerCertificate.Thumbprint -ne $c.Thumbprint) {
            throw "$([IO.Path]::GetFileName($f)) is not signed by $($c.Thumbprint)"
        }
        if (-not $s.TimeStamperCertificate) { throw "$([IO.Path]::GetFileName($f)) has no timestamp" }
        if ($s.Status -notin 'Valid', 'UnknownError') {
            throw "$([IO.Path]::GetFileName($f)): $($s.Status) $($s.StatusMessage)"
        }
    }
    Write-Pins $c.Thumbprint
    return
}

# ── Test signature ──────────────────────────────────────────────────────────
Step 'test signature'
$c = Get-SigningCert (Join-Path $out 'testcert') 'MoonlightWebVHid-test' 'MoonlightWeb Virtual HID (test)' 3650
foreach ($f in @($dll, $cat)) {
    Run $signtool @('sign', '/q', '/fd', 'sha256', '/f', $c.Pfx, '/p', $c.Password, $f)
}
Copy-Item $c.Cer $package -Force

Step "done: $package"
Get-ChildItem $package | Format-Table Name, Length -AutoSize
