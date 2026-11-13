# ============================================================================
# One click-to-photon series on the kiosk client.
#
#   powershell -NoProfile -File probe-run.ps1 -Label hevc-1080p60
#              -TargetPid <click-target.ps1's process> -BenchDisplay \\.\DISPLAYn
#              [-Clicks 10] [-SpacingMs 1500] [-ParkX 1950 -ParkY 710]
#              [-ResultsDir <path>] [-TimeoutMs 1000] [-IdleSecs 60] [-OnPrimary]
#
# The sequence is the protocol, not decoration:
#   1. nobody has used this machine's mouse or keyboard for -IdleSecs, or
#      nothing is touched: the pointer is about to move (the first series of
#      a run; the next ones, -AlreadyParked, find it on its spot or stop);
#   2. park the host pointer on the inert click target — an injected click that
#      activates another window freezes the page and hangs the probe forever;
#   3. the page is visible AND fullscreen, or the series stops — a hidden tab
#      has no rAF, so the probe would wait for a frame that never comes, and a
#      windowed canvas measures a downscale rather than the stream. Nothing
#      brings it to the front: the foreground is never taken;
#   4. ONE warm-up click, thrown away — the first click after a launch is always
#      cold (35-40 ms against a 27 ms median);
#   5. the series, ONE click per evaluation.
#
# The host clicks where the pointer is. On a machine someone uses, that may be
# their window: so before and after every click, the warm-up's included, the
# pointer must still be on the parking spot, the window under it must be the
# click target's (-TargetPid, handed over by whoever started it), and that spot
# must lie on -BenchDisplay — never the primary screen without -OnPrimary. At
# the first that is not so, the series stops: no click, no second try, no
# pointer put back. Only the rule, the window's class and its process id are
# said, never its title.
#
# Appends one JSON object per series to results/probe-results.jsonl. Discarded
# samples keep their reason, so "X:timeout" in `all` is distinguishable from a
# sample that was simply slow; a stopped series says so (invalid, stopped).
# ============================================================================
param(
    [Parameter(Mandatory = $true)] [string] $Label,
    [int] $Clicks = 10,
    [int] $SpacingMs = 1500,
    [int] $ParkX = 1950, [int] $ParkY = 710,
    [int] $DebugPort = 9333,
    [string] $ResultsDir = "$PSScriptRoot\results",
    # How long a click waits for its flag. The page's default (200 ms) cannot
    # tell a slow flag from a lost one; 1000 shows the whole distribution.
    [int] $TimeoutMs = 0,
    # The process of click-target.ps1 that drew the inert target for this run.
    [int] $TargetPid = 0,
    # The screen the parking spot is on, by its device name.
    [string] $BenchDisplay = '',
    [switch] $OnPrimary,
    # Seconds with no input on this machine before the pointer may move: up only.
    [ValidateRange(60, 3600)] [int] $IdleSecs = 60,
    # A series after the first of a run (run-browser.ps1 says so): the pointer
    # was parked then and is not moved again, and the idle check is skipped —
    # the host's own clicks of the series before reset it. The spot must still
    # be exactly as it was left before any click, or nothing happens.
    [switch] $AlreadyParked
)
$ErrorActionPreference = 'Stop'
$resultsFile = Join-Path $ResultsDir 'probe-results.jsonl'
New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null
Set-Location $PSScriptRoot

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class PrGuard {
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct MonitorInfoEx {
        public int cbSize;
        public int ml, mt, mr, mb, wl, wt, wr, wb;
        public uint dwFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szDevice;
    }
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr hwnd, uint flags);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll", EntryPoint = "GetClassNameW", CharSet = CharSet.Unicode)]
    static extern int GetClassName(IntPtr hwnd, StringBuilder name, int size);
    [DllImport("user32.dll")] static extern IntPtr MonitorFromPoint(POINT p, uint flags);
    [DllImport("user32.dll", EntryPoint = "GetMonitorInfoW", CharSet = CharSet.Unicode)]
    static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfoEx info);
    [DllImport("user32.dll")] static extern bool GetLastInputInfo(ref LASTINPUTINFO info);

    // Milliseconds since the last input of this session, from mouse or keyboard.
    // A signed difference: right across the tick counter's wrap, and an input
    // stamped a few ms ahead of the counter reads as 0, never as 49 days.
    // A failed call reads as 0 too: when in doubt, someone is there.
    public static long IdleMs() {
        LASTINPUTINFO li = new LASTINPUTINFO();
        li.cbSize = (uint)Marshal.SizeOf(typeof(LASTINPUTINFO));
        if (!GetLastInputInfo(ref li)) return 0;
        int d = unchecked(Environment.TickCount - (int)li.dwTime);
        return d < 0 ? 0 : d;
    }

    // { cursor x, cursor y, pid of the root window at (x, y), its class,
    //   the device of the screen (x, y) lies on, "1" when it is the primary }.
    public static string[] At(int x, int y) {
        POINT c;
        GetCursorPos(out c);
        POINT p; p.X = x; p.Y = y;
        IntPtr root = GetAncestor(WindowFromPoint(p), 2 /* GA_ROOT */);
        uint pid = 0;
        StringBuilder cls = new StringBuilder(256);
        if (root != IntPtr.Zero) {
            GetWindowThreadProcessId(root, out pid);
            GetClassName(root, cls, cls.Capacity);
        }
        MonitorInfoEx info = new MonitorInfoEx();
        info.cbSize = Marshal.SizeOf(typeof(MonitorInfoEx));
        IntPtr monitor = MonitorFromPoint(p, 0 /* MONITOR_DEFAULTTONULL */);
        bool ok = monitor != IntPtr.Zero && GetMonitorInfo(monitor, ref info);
        return new string[] { c.X.ToString(), c.Y.ToString(), pid.ToString(), cls.ToString(),
                              ok ? info.szDevice : "", ok && (info.dwFlags & 1) != 0 ? "1" : "0" };
    }
}
'@
if (-not [PrGuard]::SetProcessDpiAwarenessContext([IntPtr](-4))) { [PrGuard]::SetProcessDPIAware() | Out-Null }

function Write-Stopped([string] $why) {
    # The series did not run, or stopped: a line that says so, nothing else.
    $line = @{ label = $Label; ts = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds(); n = 0; of = 0
               all = @(); samples = @(); invalid = $true; stopped = $why } | ConvertTo-Json -Compress
    Add-Content -Path $resultsFile -Value $line -Encoding UTF8
    $line
}

# '' when the pointer is on the parking spot, over the click target's window,
# on the bench screen; otherwise which rule failed, with the class and the
# process id of the window there.
function Test-Spot {
    $s = [PrGuard]::At($ParkX, $ParkY)
    $why = if ([int]$s[0] -ne $ParkX -or [int]$s[1] -ne $ParkY) { 'the pointer moved' }
           elseif ([int]$s[2] -ne $TargetPid) { 'another window is under the pointer' }
           elseif ($s[4] -ne $BenchDisplay) { 'the spot is on another screen' }
           elseif ($s[5] -eq '1' -and -not $OnPrimary) { 'the spot is on the primary screen' }
           else { '' }
    if (-not $why) { return '' }
    return "$why (class $($s[3]), pid $($s[2]))"
}

# -- Before anything moves ----------------------------------------------------
if ($TargetPid -le 0 -or -not $BenchDisplay) {
    Write-Stopped 'no -TargetPid or no -BenchDisplay: the clicks would land unchecked'; return
}
$target = Get-CimInstance Win32_Process -Filter "ProcessId=$TargetPid" -ErrorAction SilentlyContinue
if (-not $target -or $target.CommandLine -notmatch 'click-target\.ps1') {
    Write-Stopped "process $TargetPid is not click-target.ps1"; return
}
if ($AlreadyParked) {
    # Nobody touched the mouse since the series before: the pointer is still
    # exactly on its spot, over the target. Checked now, before anything else.
    $bad = Test-Spot
    if ($bad) { Write-Stopped "the parked pointer: $bad"; return }
} else {
    $idle = [PrGuard]::IdleMs()
    if ($idle -lt $IdleSecs * 1000) {
        Write-Stopped ("this machine was used {0:N0} s ago (under {1} s): the pointer is not moved" -f ($idle / 1000), $IdleSecs)
        return
    }
    [PrGuard]::SetCursorPos($ParkX, $ParkY) | Out-Null
    Start-Sleep -Milliseconds 300
}

$state = python cdp.py --port $DebugPort eval "document.visibilityState + ' fs=' + !!document.fullscreenElement"
"visibility: $state"
if ($state -notmatch '^visible fs=(True|true)') {
    Write-Stopped 'the client page is not visible and fullscreen: nothing brings it to the front'; return
}

# No probe on the page at all: the launch reply did not carry latency_flag,
# which is what happens whenever the host's settings.json lacks
# "latency_flag_enabled": true (default false, read at startup — a fresh
# install resets it). Every series then came back as a bare {}: no label, no
# reason, nothing to read (25/09/2026). Say so and write a line that says it.
$has = python cdp.py --port $DebugPort eval "typeof window.mwLatency === 'object' && window.mwLatency !== null"
if ($has -notmatch 'true') {
    Write-Warning ("no click-to-photon probe on the page: the host did not raise latency_flag. " +
                   "Set `"latency_flag_enabled`": true in the settings.json the host reads, and restart it.")
    $none = @{ label = $Label; ts = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds(); n = 0; of = 0
               all = @(); samples = @(); error = 'no probe: latency_flag_enabled is off on the host' } |
            ConvertTo-Json -Compress
    Add-Content -Path $resultsFile -Value $none -Encoding UTF8
    return
}

# One click, checked before and after. Returns the page's entry for it, or a
# string: why the series stops here.
$runOpts = if ($TimeoutMs -gt 0) { "{ timeoutMs: $TimeoutMs }" } else { '{}' }
$evalTimeout = [math]::Ceiling([math]::Max($TimeoutMs, 200) / 1000) + 30
function Invoke-OneClick {
    $bad = Test-Spot
    if ($bad) { return "before a click: $bad" }
    $out = python cdp.py --port $DebugPort --timeout $evalTimeout eval "(async()=>JSON.stringify(await mwLatency.run(1, 0, $runOpts)))()"
    $bad = Test-Spot
    if ($bad) { return "during a click, not counted: $bad" }
    $entry = $null
    if ($out) { try { $entry = @($out | ConvertFrom-Json)[0] } catch { } }
    if ($null -eq $entry -or $entry -is [string]) { return 'the page did not answer the click' }
    return $entry
}

$warm = Invoke-OneClick
if ($warm -is [string]) { Write-Stopped "warm-up: $warm"; return }
"warm-up (discarded): ok=$($warm.ok)"
Start-Sleep -Milliseconds 800

$entries = @()
$stopped = ''
for ($i = 0; $i -lt $Clicks; $i++) {
    if ($i -gt 0) { Start-Sleep -Milliseconds $SpacingMs }
    $e = Invoke-OneClick
    if ($e -is [string]) { $stopped = $e; break }
    $entries += $e
}

# The summary the page used to make, from the clicks this series kept.
$ok = @($entries | Where-Object { $_.ok } | ForEach-Object { [double]$_.latencyMs } | Sort-Object)
function At([double] $p) {
    if (-not $ok.Count) { return $null }
    $ok[[math]::Min($ok.Count - 1, [math]::Max(0, [math]::Ceiling($p * $ok.Count) - 1))]
}
$series = [ordered]@{
    label = $Label; ts = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    timeoutMs = $(if ($TimeoutMs -gt 0) { $TimeoutMs } else { 200 })
    n = $ok.Count; of = $entries.Count
    median = (At 0.5); p90 = (At 0.9); p99 = (At 0.99)
    min = $(if ($ok.Count) { $ok[0] } else { $null }); max = $(if ($ok.Count) { $ok[-1] } else { $null })
    all = @($entries | ForEach-Object { if ($_.ok) { $_.latencyMs } else { 'X:' + $_.reason } })
    samples = $ok
}
if ($stopped) { $series.invalid = $true; $series.stopped = $stopped }
$out = $series | ConvertTo-Json -Compress
$out
Add-Content -Path $resultsFile -Value $out -Encoding UTF8
