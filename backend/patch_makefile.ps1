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

# Fix link response: line ending with "...moc_StreamRelay.obj" followed by newline + "$(LIBS)"
$pattern1 = '(release\\moc_StreamRelay\.obj)\r?\n\$\(LIBS\)'
$replacement1 = '$1 release\moc_MoonlightShim.obj' + "`r`n" + '$(LIBS)'
$content = $content -replace $pattern1, $replacement1

# Fix clean rule: only if moc_MoonlightShim.obj not already present in clean rule
if ($content -notmatch 'clean:.*moc_MoonlightShim\.obj') {
    $pattern2 = '(release\\moc_StreamRelay\.obj)(\r?\n\s+-?\$\(DEL_FILE\))'
    $replacement2 = '$1 release\moc_MoonlightShim.obj$2'
    $content = $content -replace $pattern2, $replacement2
}

if ($content -ne $original) {
    [System.IO.File]::WriteAllText((Resolve-Path $Makefile).Path, $content)
    Write-Host "[PATCH] ${Makefile}: added moc_MoonlightShim.obj"
} else {
    Write-Host "[PATCH] ${Makefile}: already patched"
}
