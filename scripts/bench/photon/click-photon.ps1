# The client's half of the click → photon bench (POC Ultra U0.4), without a
# camera: clicks into a streaming client's window and times, on this machine's
# own screen, how long the host's flag-window.ps1 takes to flip there. The same
# measure for any client: Steam Remote Play (HEVC or PyroWave), MoonlightWeb in
# Chrome.
#
#   powershell -NoProfile -File click-photon.ps1 -Process streaming_client -Clicks 60 -Out steam-hevc.json
#   powershell -NoProfile -File click-photon.ps1 -Process chrome -Title MoonlightWeb -Out mw.json
#
# A click is injected (SendInput) at the window's centre, then the pixel there
# is read back from the composed desktop (GDI, the DWM's last composed frame)
# until its brightness moves by more than half the range. Time: the click's
# send to the first changed read, QueryPerformanceCounter. What it includes:
# the way up, the host's flip, its capture, encode, network, decode, the
# client's present and the DWM's composition, plus up to one read interval
# (~1 ms). What it leaves out: the screen's own scan-out and response, the same
# for every client on one screen.
# The mouse must stay still meanwhile; the window must not be covered.
#
# One window only: when several match (a second Chrome titled MoonlightWeb, the
# user's own, was open on the UM790Pro on 07/11/2026), it stops instead of
# picking the first; -CdpPort 9222 names the bench Chrome by the port it is
# driven on, -ProcessId by its pid. Before every click the window must be in
# front and under the pointer, or no click is sent and the run stops there
# (`guard` in the JSON says why).
param(
    [string] $Process = 'streaming_client',
    [string] $Title = '',
    [int] $CdpPort = 0,
    [int] $ProcessId = 0,
    [int] $Clicks = 60,
    [int] $IntervalMs = 700,
    [int] $TimeoutMs = 1500,
    # The pixel read, from the click: MoonlightWeb's bench flag
    # (latency_flag_enabled) is drawn where the click lands and would hide it.
    [int] $ReadDx = 0,
    [int] $ReadDy = 0,
    [string] $Out = ''
)
Add-Type @"
using System; using System.Diagnostics; using System.Runtime.InteropServices; using System.Threading;
public static class Photon {
  [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr h);
  [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr h, IntPtr dc);
  [DllImport("gdi32.dll")] static extern uint GetPixel(IntPtr dc, int x, int y);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] i, int size);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr h, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public MOUSEINPUT mi; }
  public static void Init() { SetProcessDpiAwarenessContext((IntPtr)(-4)); }
  public static int Luma(int x, int y) {
    IntPtr dc = GetDC(IntPtr.Zero); uint c = GetPixel(dc, x, y); ReleaseDC(IntPtr.Zero, dc);
    int r = (int)(c & 0xff), g = (int)((c >> 8) & 0xff), b = (int)((c >> 16) & 0xff);
    return (r * 3 + g * 6 + b) / 10;
  }
  static void Click() {
    var i = new INPUT[2];
    i[0].type = 0; i[0].mi.flags = 0x0002;  // LEFTDOWN
    i[1].type = 0; i[1].mi.flags = 0x0004;  // LEFTUP
    SendInput(2, i, Marshal.SizeOf(typeof(INPUT)));
  }
  // 0 if the target is in front and under (x, y); 1 another window is in front,
  // 2 another window is under the pointer.
  public static int Guard(IntPtr target, int x, int y) {
    if (GetForegroundWindow() != target) return 1;
    POINT p; p.X = x; p.Y = y;
    if (GetAncestor(WindowFromPoint(p), 2) != target) return 2;  // GA_ROOT
    return 0;
  }
  // One click; ms until the pixel at (x, y) moved past half the range, -1 if it
  // did not, or -10 minus the guard's reason when no click was sent.
  public static double Once(IntPtr target, int x, int y, int rx, int ry, int timeoutMs) {
    SetCursorPos(x, y);
    Thread.Sleep(30);
    int before = Luma(rx, ry);
    int g = Guard(target, x, y);
    if (g != 0) return -10 - g;
    long t0 = Stopwatch.GetTimestamp();
    Click();
    long end = t0 + (long)timeoutMs * Stopwatch.Frequency / 1000;
    while (Stopwatch.GetTimestamp() < end) {
      if (Math.Abs(Luma(rx, ry) - before) > 127)
        return (Stopwatch.GetTimestamp() - t0) * 1000.0 / Stopwatch.Frequency;
    }
    return -1;
  }
}
"@
[Photon]::Init()
$want = "process '$Process'$(if ($Title) { " titled *$Title*" })"
if ($ProcessId -eq 0 -and $CdpPort -gt 0) {
    $l = Get-NetTCPConnection -LocalPort $CdpPort -State Listen -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $l) { throw "nothing listens on CDP port ${CdpPort}: no window to click" }
    $ProcessId = [int]$l.OwningProcess
    $want += " holding CDP port $CdpPort"
}
$procs = @(Get-Process -Name $Process -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 })
if ($Title) { $procs = @($procs | Where-Object { $_.MainWindowTitle -like "*$Title*" }) }
if ($ProcessId -gt 0) { $procs = @($procs | Where-Object { $_.Id -eq $ProcessId }); $want += " (pid $ProcessId)" }
if ($procs.Count -eq 0) { throw "no window of $want" }
if ($procs.Count -gt 1) {
    throw "$($procs.Count) windows of $want (pids $(($procs | ForEach-Object Id) -join ', ')): name one with -CdpPort or -ProcessId; nothing clicked"
}
$p = $procs[0]
$h = $p.MainWindowHandle
$r = New-Object Photon+RECT
[void][Photon]::GetWindowRect($h, [ref]$r)
$x = [int](($r.L + $r.R) / 2); $y = [int](($r.T + $r.B) / 2)
"window '$($p.MainWindowTitle)' ($($p.ProcessName) $($p.Id)) at $($r.L),$($r.T)-$($r.R),$($r.B); clicks at $x,$y, reads at $($x + $ReadDx),$($y + $ReadDy)"
[void][Photon]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 500
$samples = New-Object System.Collections.Generic.List[double]
$missed = 0; $sent = 0; $guard = ''
for ($i = 0; $i -lt $Clicks; $i++) {
    $ms = [Photon]::Once($h, $x, $y, $x + $ReadDx, $y + $ReadDy, $TimeoutMs)
    if ($ms -le -10) {
        $why = if ($ms -eq -11) { 'another window is in front' } else { 'another window is under the pointer' }
        $guard = "click $($i + 1): $why; not sent, run stopped"
        break
    }
    $sent++
    if ($ms -lt 0) { $missed++ } else { $samples.Add([math]::Round($ms, 2)) }
    Start-Sleep -Milliseconds ([math]::Max(50, $IntervalMs - [int][math]::Max(0, $ms)))
}
$sorted = @($samples | Sort-Object)
function Pct($q) { if ($sorted.Count) { $sorted[[math]::Min($sorted.Count - 1, [int][math]::Floor($q * $sorted.Count))] } else { $null } }
$summary = [ordered]@{
    process = $p.ProcessName; pid = $p.Id; title = $p.MainWindowTitle; clicks = $Clicks; sent = $sent; measured = $sorted.Count; missed = $missed
    medianMs = (Pct 0.5); p90Ms = (Pct 0.9); minMs = $(if ($sorted.Count) { $sorted[0] }); maxMs = $(if ($sorted.Count) { $sorted[-1] })
    guard = $guard; samples = $samples; at = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
}
"click -> photon: $($summary.measured) of $Clicks measured ($sent sent), median $($summary.medianMs) ms, p90 $($summary.p90Ms) ms ($($summary.minMs) to $($summary.maxMs)), $missed missed"
if ($guard) { "GUARD: $guard" }
if ($Out) { $summary | ConvertTo-Json -Depth 3 | Set-Content -Path $Out -Encoding utf8 }
