# Patch qmake-generated Makefile to include moc_MoonlightShim.obj
# Workaround for qmake MSVC generator bug: MOC rule is generated but
# the resulting .obj is omitted from the link response file and clean rule.
param(
    [string]$Makefile = "Makefile.Release"
)

if (-not (Test-Path $Makefile)) {
    Write-Host "[PATCH] ${Makefile} not found, skipping"
    exit 0
}

$content = Get-Content $Makefile -Raw
$original = $content

# StreamRelay.h has been removed (replaced by DataChannelRelay + SignalingServer).
# qmake may still drop moc_MoonlightShim.obj from the link response (MSVC bug).
# Check for missing moc_MoonlightShim.obj in link line and add if needed.
if ($content -notmatch 'moc_MoonlightShim\.obj') {
    # Find the last moc_*.obj and add moc_MoonlightShim.obj after it
    $pattern1 = '(release\\moc_\w+\.obj)(\r?\n\s+\$\(LIBS\))'
    $replacement1 = '$1 release\moc_MoonlightShim.obj$2'
    $newContent = $content -replace $pattern1, $replacement1

    if ($newContent -ne $content) {
        $content = $newContent

        # Fix clean rule too — match the last moc_*.obj before a DEL_FILE line
        $pattern2 = '(release\\moc_\w+\.obj)(\r?\n\s+-?\$\(DEL_FILE\))'
        $replacement2 = '$1 release\moc_MoonlightShim.obj$2'
        $content = $content -replace $pattern2, $replacement2
    }
}

if ($content -ne $original) {
    [System.IO.File]::WriteAllText((Resolve-Path $Makefile).Path, $content)
    Write-Host "[PATCH] ${Makefile}: added moc_MoonlightShim.obj"
} else {
    Write-Host "[PATCH] ${Makefile}: already patched (or not needed)"
}
