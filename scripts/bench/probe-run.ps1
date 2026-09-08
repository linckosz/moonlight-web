# ============================================================================
# One click-to-photon series on the kiosk client.
#
#   powershell -NoProfile -File probe-run.ps1 -Label hevc-1080p60
#              [-Clicks 10] [-SpacingMs 1500] [-ParkX 1950 -ParkY 710]
#              [-ResultsDir <path>]
#
# The sequence is the protocol, not decoration:
#   1. park the host pointer on the inert click target — an injected click that
#      activates another window freezes the page and hangs the probe forever;
#   2. Page.bringToFront and assert the page is visible AND fullscreen — a
#      hidden tab has no rAF, so the probe would wait for a frame that never
#      comes, and a windowed canvas measures a downscale rather than the stream;
#   3. ONE warm-up click, thrown away — the first click after a launch is always
#      cold (35-40 ms against a 27 ms median);
#   4. the series.
#
# Appends one JSON object per series to results/probe-results.jsonl. Discarded
# samples keep their reason, so "X:timeout" in `all` is distinguishable from a
# sample that was simply slow.
# ============================================================================
param(
    [Parameter(Mandatory = $true)] [string] $Label,
    [int] $Clicks = 10,
    [int] $SpacingMs = 1500,
    [int] $ParkX = 1950, [int] $ParkY = 710,
    [int] $DebugPort = 9333,
    [string] $ResultsDir = "$PSScriptRoot\results"
)
Add-Type -Namespace PR -Name U -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c); [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);'
if (-not [PR.U]::SetProcessDpiAwarenessContext([IntPtr](-4))) { [PR.U]::SetProcessDPIAware() | Out-Null }

New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null
Set-Location $PSScriptRoot

[PR.U]::SetCursorPos($ParkX, $ParkY) | Out-Null
Start-Sleep -Milliseconds 300
python cdp.py --port $DebugPort call Page.bringToFront | Out-Null
Start-Sleep -Milliseconds 500

$state = python cdp.py --port $DebugPort eval "document.visibilityState + ' fs=' + !!document.fullscreenElement"
"visibility: $state"
if ($state -notlike 'visible*') {
    Write-Warning "the client page is not visible — the probe would hang; fix that before measuring"
}

$warm = python cdp.py --port $DebugPort eval "(async()=>{const e=await mwLatency.run(1,500); return JSON.stringify(e);})()"
"warm-up (discarded): $warm"
Start-Sleep -Milliseconds 800
[PR.U]::SetCursorPos($ParkX, $ParkY) | Out-Null

$js = @"
(async()=>{
  const e = await mwLatency.run($Clicks, $SpacingMs);
  const ok = e.filter(x=>x.ok).map(x=>x.latencyMs).sort((a,b)=>a-b);
  const at = p => ok[Math.min(ok.length-1, Math.max(0, Math.ceil(p*ok.length)-1))];
  return JSON.stringify({
    label: '$Label', ts: Date.now(),
    n: ok.length, of: e.length,
    median: at(0.5), p90: at(0.9), p99: at(0.99),
    min: ok[0], max: ok[ok.length-1],
    all: e.map(x => x.ok ? x.latencyMs : ('X:' + x.reason)),
    samples: ok
  });
})()
"@
$out = python cdp.py --port $DebugPort eval $js
$out
Add-Content -Path (Join-Path $ResultsDir 'probe-results.jsonl') -Value $out -Encoding UTF8
