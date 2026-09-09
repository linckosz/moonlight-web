# ============================================================================
# Put the bench content on the bench display, and make sure it is what the
# display SHOWS: kill the previous kiosk Chrome, launch a new one on the URL,
# wait for its window, pin it TOPMOST and stretch it over the display's physical
# rectangle.
#
#   .\kiosk.ps1 -Url file:///.../cod.html [-X 0 -Y 0 -W 2560 -H 1440]
#               [-DebugPort 9333] [-Verify]
#
# Why each of those is not optional, each learned the hard way:
#
#   - Other windows left on that display would otherwise be what the bench
#     captures. A near-static desktop encodes at 14 KB and QP 10 and looks like
#     a wonderful result; it is measuring nothing.
#   - Per-monitor DPI v2 or the rectangle is virtualised and the window lands on
#     the wrong pixels of a display whose scale differs from the primary's.
#   - TOPMOST is re-asserted twice: the first SetWindowPos sometimes races
#     Chrome's own sizing of a kiosk window.
#   - -DebugPort turns this into the CLIENT kiosk (the app, not the content):
#     the same window management, plus the remote debugging port cdp.py drives.
#     The Chrome extension cannot be used for this — it emulates a fixed
#     viewport, its clicks never carry a user activation so requestFullscreen is
#     refused, and its tab stays visibilityState=hidden, which freezes rAF and
#     hangs the probe.
# ============================================================================
param(
    [Parameter(Mandatory = $true)] [string] $Url,
    [int] $X = 0, [int] $Y = 0, [int] $W = 2560, [int] $H = 1440,
    [string] $ChromeProfile = '',
    [int] $DebugPort = 0,
    [switch] $Verify
)
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices; using System.Collections.Generic;
public struct KRECT { public int Left, Top, Right, Bottom; }
public class Kiosk {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out KRECT r);
  public static IntPtr Largest(HashSet<uint> pids) {
    IntPtr best = IntPtr.Zero; long bestArea = 0;
    EnumWindows((h, l) => {
      if (!IsWindowVisible(h)) return true;
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (!pids.Contains(pid)) return true;
      var c = new StringBuilder(64); GetClassName(h, c, 64);
      if (c.ToString() != "Chrome_WidgetWin_1") return true;
      KRECT r; GetWindowRect(h, out r);
      long area = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
      if (area > bestArea) { bestArea = area; best = h; }
      return true;
    }, IntPtr.Zero);
    return best;
  }
}
"@
# Per-monitor v2 (-4): the rectangle is physical on a display whose scale differs from the primary's.
if (-not [Kiosk]::SetProcessDpiAwarenessContext([IntPtr](-4))) { [Kiosk]::SetProcessDPIAware() | Out-Null }

# The profile is resolved HERE, not as a parameter default: $PSScriptRoot has
# been seen coming back empty while binding this script's parameters, which made
# the path collapse to \.chrome-bench at the root of whatever drive was current.
# Chrome then started, found nothing it owned there, exited 0 without ever
# opening a window, and every pass of a campaign measured a motionless desktop.
# In the body $PSScriptRoot is reliable.
# Content and client are two kiosks that must live side by side, and the launch
# below kills every Chrome holding THIS profile. Sharing one profile therefore
# made the second kiosk kill the first: the client came up and the reference
# clip vanished from the captured screen, which is indistinguishable from a
# campaign that simply measured a desktop. -DebugPort is what separates them
# (only the client is driven over CDP), so it picks the profile too.
if (-not $ChromeProfile) {
    $ChromeProfile = Join-Path $PSScriptRoot ($(if ($DebugPort -gt 0) { '.chrome-client' } else { '.chrome-bench' }))
}
if (-not [System.IO.Path]::IsPathRooted($ChromeProfile)) {
    $ChromeProfile = Join-Path $PSScriptRoot $ChromeProfile
}

$profileTag = Split-Path $ChromeProfile -Leaf
Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" |
    Where-Object { $_.CommandLine -like "*$profileTag*" } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Milliseconds 800

$chrome = "C:\Program Files\Google\Chrome\Application\chrome.exe"
if (-not (Test-Path $chrome)) { throw "Chrome not found at $chrome" }

$chromeArgs = @(
    "--user-data-dir=$ChromeProfile", "--no-first-run", "--no-default-browser-check",
    "--disable-infobars", "--autoplay-policy=no-user-gesture-required", "--mute-audio", "--kiosk",
    "--window-position=$X,$Y"
)
if ($DebugPort -gt 0) {
    # The client kiosk talks to a dev instance over HTTPS with a self-signed
    # certificate; without this the page is an interstitial nothing can pass.
    $chromeArgs += "--remote-debugging-port=$DebugPort"
    $chromeArgs += "--ignore-certificate-errors"
    $chromeArgs += "--unsafely-treat-insecure-origin-as-secure=$Url"
}
$chromeArgs += $Url
Start-Process -FilePath $chrome -ArgumentList $chromeArgs

$h = [IntPtr]::Zero
for ($i = 0; $i -lt 40 -and $h -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 250
    $pids = New-Object 'System.Collections.Generic.HashSet[uint32]'
    Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" |
        Where-Object { $_.CommandLine -like "*$profileTag*" } |
        ForEach-Object { [void]$pids.Add([uint32]$_.ProcessId) }
    $h = [Kiosk]::Largest($pids)
}
if ($h -eq [IntPtr]::Zero) { throw "kiosk window never appeared" }
# HWND_TOPMOST (-1), exact physical rectangle, SWP_SHOWWINDOW.
[Kiosk]::SetWindowPos($h, [IntPtr](-1), $X, $Y, $W, $H, 0x0040) | Out-Null
Start-Sleep -Milliseconds 300
[Kiosk]::SetWindowPos($h, [IntPtr](-1), $X, $Y, $W, $H, 0x0040) | Out-Null
$r = New-Object KRECT
[Kiosk]::GetWindowRect($h, [ref]$r) | Out-Null
"kiosk window $h at $($r.Left),$($r.Top) -> $($r.Right),$($r.Bottom), topmost"

if ($Verify) {
    Add-Type -AssemblyName System.Drawing
    Start-Sleep -Seconds 4
    $bmp = New-Object System.Drawing.Bitmap -ArgumentList $W, $H
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($X, $Y, 0, 0, $bmp.Size)
    $small = New-Object System.Drawing.Bitmap -ArgumentList $bmp, ([int]($W / 2)), ([int]($H / 2))
    $small.Save("$PSScriptRoot\kiosk-verify.png")
    "saved $PSScriptRoot\kiosk-verify.png"
}
