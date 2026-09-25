#requires -version 5
<#
.SYNOPSIS
    Fails when an ARM64 binary has a function that calls __chkstk before
    saving LR — a function that returns into its own prologue.

.DESCRIPTION
    MSVC 14.51 (Visual Studio 18) emits such prologues when it shrink-wraps a
    function that also needs a stack probe: `bl __chkstk` overwrites LR, the
    LR saved a few instructions later is the return address of that bl, and
    the function's `ret` lands back in its prologue. OpenSSL, whose Windows
    targets probe every function (/Gs0), crashed that way on the first TLS
    handshake. See docs/design/openssl-windows.md.

    Functions are delimited by the labels dumpbin prints from the PDB, which
    must sit beside each binary. dumpbin comes from PATH (a developer prompt)
    or from the newest Visual Studio found.

.EXAMPLE
    scripts\check-arm64-chkstk.ps1 build\MoonlightWeb.exe backend\libs\windows\lib\arm64\*.dll
#>
param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]]$Path
)
$ErrorActionPreference = 'Stop'

$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
if (-not $dumpbin) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -property installationPath
    $dumpbin = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC\*\bin\Host*\*\dumpbin.exe') -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
    if (-not $dumpbin) { throw 'dumpbin.exe not found: run from a Visual Studio developer prompt' }
}

$failed = $false
foreach ($file in (Resolve-Path $Path)) {
    $text = [IO.Path]::GetTempFileName()
    try {
        # Through cmd: the disassembly of a large DLL is tens of MB, far faster
        # to write to a file than to stream through the PowerShell pipeline.
        & cmd.exe /c "`"$dumpbin`" /nologo /disasm `"$file`" > `"$text`""
        if ($LASTEXITCODE -ne 0) { throw "dumpbin failed on $file" }

        $func = $null; $lrSaved = $false; $calls = 0; $bad = @(); $labels = 0
        switch -Regex -File $text {
            '^(\S.*):$' { $func = $Matches[1]; $lrSaved = $false; $labels++; continue }
            # "  0000000180001000: F9400A51  str         lr,[sp,#0x50]"
            '^\s+[0-9A-F]+: [0-9A-F]{8}\s+(st[rp])\s+([^\[]*)' {
                if ($Matches[2] -match '\blr\b') { $lrSaved = $true }
                continue
            }
            '^\s+[0-9A-F]+: [0-9A-F]{8}\s+bl\s+.*__chkstk' {
                $calls++
                if ($func -and -not $lrSaved) { $bad += $func }
            }
        }
        if ($labels -lt 2) {
            # Without the PDB there are no labels, hence no functions to judge:
            # a clean result would mean nothing.
            Write-Host "$file`: no symbols (is its PDB beside it?) - cannot check"
            $failed = $true
            continue
        }
        Write-Host "$file`: $calls __chkstk call(s), $($bad.Count) before LR is saved"
        $bad | Select-Object -First 20 | ForEach-Object { Write-Host "  $_" }
        if ($bad.Count -gt 20) { Write-Host "  ... and $($bad.Count - 20) more" }
        if ($bad.Count) { $failed = $true }
    } finally {
        Remove-Item $text -ErrorAction SilentlyContinue
    }
}
if ($failed) { exit 1 }
exit 0
