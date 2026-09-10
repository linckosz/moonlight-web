# ============================================================================
# The way out of the kiosk, for someone sitting in front of the machine.
#
#   .\kiosk-hotkeys.ps1              (started by kiosk.ps1, hidden, no window)
#
#   Ctrl+Alt+Shift+M   un-pin every bench kiosk and minimise it
#   Ctrl+Alt+Shift+Q   close every bench kiosk and its helpers
#
# Why a system-wide hotkey and not a key handled by the page:
#
#   - The kiosk is TOPMOST and borderless. Alt+F4 closes the FOCUSED window,
#     which during a campaign is the client kiosk or something hidden behind
#     the overlay - you close a window you cannot see, and the video stays.
#   - The content page is what the encoder is measuring; giving it a key
#     handler would mean a keystroke arriving from the stream could stop the
#     bench mid-pass. RegisterHotKey is host-side and never reaches the page.
#   - Three modifiers, deliberately: a streamed game presses Q, M, Ctrl+Q and
#     Alt+M. It does not press this.
#
# M before Q on purpose: minimising also drops HWND_TOPMOST, so the screen is
# usable again while the campaign keeps its Chrome alive and resumable.
# ============================================================================
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices; using System.Collections.Generic;
public struct HRECT { public int Left, Top, Right, Bottom; }
public class KHot {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool RegisterHotKey(IntPtr h, int id, uint mods, uint vk);
  [DllImport("user32.dll")] public static extern bool UnregisterHotKey(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool PeekMessage(out MSG m, IntPtr h, uint min, uint max, uint flag);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [StructLayout(LayoutKind.Sequential)] public struct MSG {
    public IntPtr hwnd; public uint message; public IntPtr wParam, lParam; public uint time; public int x, y;
  }
  public static List<IntPtr> WindowsOf(HashSet<uint> pids) {
    var found = new List<IntPtr>();
    EnumWindows((h, l) => {
      if (!IsWindowVisible(h)) return true;
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (!pids.Contains(pid)) return true;
      var c = new StringBuilder(64); GetClassName(h, c, 64);
      if (c.ToString() == "Chrome_WidgetWin_1") found.Add(h);
      return true;
    }, IntPtr.Zero);
    return found;
  }
}
"@

$TAGS = @('.chrome-bench', '.chrome-client')

function Get-KioskPids {
    $pids = New-Object 'System.Collections.Generic.HashSet[uint32]'
    Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" -ErrorAction SilentlyContinue | ForEach-Object {
        $cmd = $_.CommandLine
        foreach ($t in $TAGS) { if ($cmd -like "*$t*") { [void]$pids.Add([uint32]$_.ProcessId); break } }
    }
    return $pids
}

# MOD_ALT 1 | MOD_CONTROL 2 | MOD_SHIFT 4 | MOD_NOREPEAT 0x4000
$mods = 1 -bor 2 -bor 4 -bor 0x4000
$ID_MIN = 1
$ID_QUIT = 2
$okM = [KHot]::RegisterHotKey([IntPtr]::Zero, $ID_MIN, $mods, 0x4D)   # M
$okQ = [KHot]::RegisterHotKey([IntPtr]::Zero, $ID_QUIT, $mods, 0x51)  # Q

# This process is started hidden, so anything it writes to a console is written
# to nothing. A warning nobody can read is not a warning: the one fact that
# decides whether the escape hatch exists goes to a file.
$log = Join-Path $env:TEMP 'mw-kiosk-hotkeys.log'
"$(Get-Date -Format 'HH:mm:ss') pid $PID  minimise=$okM  close=$okQ" | Set-Content -Path $log -Encoding ASCII
if (-not ($okM -and $okQ)) {
    # Somebody else owns the combination. Stay alive anyway: the hotkey is a
    # convenience, kiosk-close.ps1 is the guarantee.
    "  at least one hotkey is held by another application - use kiosk-close.ps1" |
        Add-Content -Path $log -Encoding ASCII
}

$idle = 0
while ($true) {
    $msg = New-Object KHot+MSG
    # PM_REMOVE (1), WM_HOTKEY (0x0312) only. hWnd NULL: the messages land on
    # this thread's own queue, which PeekMessage creates on first call.
    while ([KHot]::PeekMessage([ref]$msg, [IntPtr]::Zero, 0x0312, 0x0312, 1)) {
        $which = [int]$msg.wParam
        if ($which -eq $ID_MIN) {
            $wins = [KHot]::WindowsOf((Get-KioskPids))
            foreach ($w in $wins) {
                # HWND_NOTOPMOST (-2), SWP_NOMOVE|SWP_NOSIZE (0x0002|0x0001).
                # Un-pin FIRST: a minimised window that is still TOPMOST comes
                # back over everything the moment anything restores it.
                [void][KHot]::SetWindowPos($w, [IntPtr](-2), 0, 0, 0, 0, 0x0003)
                [void][KHot]::ShowWindow($w, 6)   # SW_MINIMIZE
            }
        }
        elseif ($which -eq $ID_QUIT) {
            & powershell -NoProfile -File (Join-Path $PSScriptRoot 'kiosk-close.ps1') | Out-Null
            [void][KHot]::UnregisterHotKey([IntPtr]::Zero, $ID_MIN)
            [void][KHot]::UnregisterHotKey([IntPtr]::Zero, $ID_QUIT)
            return
        }
    }
    Start-Sleep -Milliseconds 120
    # Outlive nothing: once no kiosk is left, this process has no reason to
    # hold a global hotkey. Ten seconds of grace so it survives the gap while
    # kiosk.ps1 restarts a kiosk between two passes.
    if ((Get-KioskPids).Count -eq 0) { $idle++ } else { $idle = 0 }
    if ($idle -gt 83) {
        [void][KHot]::UnregisterHotKey([IntPtr]::Zero, $ID_MIN)
        [void][KHot]::UnregisterHotKey([IntPtr]::Zero, $ID_QUIT)
        return
    }
}
