# The host's half of the click → photon bench (POC Ultra U0.4): a window over a
# whole screen that flips between black and white on every mouse button press,
# at once, in its own message loop. Streamed by any client (Steam Remote Play,
# MoonlightWeb), it shows the click's answer; click-photon.ps1 on the client
# times it.
#
# For Steam, add it as a non-Steam game: target powershell.exe, launch options
#   -NoProfile -ExecutionPolicy Bypass -File "<this file>"
#
#   powershell -NoProfile -File flag-window.ps1 [-Screen 0] [-Log flips.csv]
#
# -Screen: the index in [Screen]::AllScreens (default: the primary screen).
# -Log: one line per flip, the host's QueryPerformanceCounter time in µs.
# Esc closes it.
param([int] $Screen = -1, [string] $Log = '')
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing @"
using System; using System.IO; using System.Diagnostics; using System.Windows.Forms; using System.Drawing;
using System.Runtime.InteropServices;
public class FlagForm : Form {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  bool white; StreamWriter log;
  public FlagForm(Rectangle area, string logPath) {
    FormBorderStyle = FormBorderStyle.None; TopMost = true; StartPosition = FormStartPosition.Manual;
    Bounds = area; BackColor = Color.Black; Text = "MW click-photon flag"; Cursor = Cursors.Cross;
    DoubleBuffered = true;
    if (logPath.Length > 0) { log = new StreamWriter(logPath, true); log.AutoFlush = true; }
  }
  void Flip() {
    white = !white;
    BackColor = white ? Color.White : Color.Black;
    Invalidate(); Update();   // painted now, not at the next idle
    if (log != null) log.WriteLine("{0},{1}", Stopwatch.GetTimestamp() * 1000000 / Stopwatch.Frequency, white ? 1 : 0);
  }
  protected override void OnMouseDown(MouseEventArgs e) { Flip(); }
  protected override void OnKeyDown(KeyEventArgs e) { if (e.KeyCode == Keys.Escape) Close(); }
}
"@
# Per-monitor aware BEFORE any bounds are read: read first, at 125 % scaling the
# screen is 2048x1152 logical pixels, and the flag then covered only 2048x1152
# physical ones of a 2560x1440 screen (DualRTX, 07/11/2026).
[void][FlagForm]::SetProcessDpiAwarenessContext([IntPtr](-4))
$screens = [System.Windows.Forms.Screen]::AllScreens
$s = if ($Screen -ge 0 -and $Screen -lt $screens.Count) { $screens[$Screen] } else { [System.Windows.Forms.Screen]::PrimaryScreen }
$f = New-Object FlagForm $s.Bounds, $Log
[System.Windows.Forms.Application]::Run($f)
