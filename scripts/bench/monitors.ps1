# Physical monitor rectangles, DPI-aware from the first instruction so nothing
# is virtualised. The kiosk and the click target are placed with these numbers,
# and a rectangle scaled to the primary monitor's DPI would put the bench window
# on the wrong pixels of a display whose scale differs.
#
# Run in a fresh process — DPI awareness is per-process and cannot be undone:
#   powershell -NoProfile -File monitors.ps1
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
[StructLayout(LayoutKind.Sequential)] public struct MRECT { public int Left, Top, Right, Bottom; }
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct MONINFO { public int cbSize; public MRECT rcMonitor; public MRECT rcWork; public uint dwFlags;
  [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szDevice; }
public class Mon {
  public delegate bool EnumProc(IntPtr h, IntPtr hdc, ref MRECT r, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
  [DllImport("user32.dll")] public static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, EnumProc p, IntPtr l);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool GetMonitorInfo(IntPtr h, ref MONINFO mi);
  public static void List() {
    EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (IntPtr h, IntPtr hdc, ref MRECT r, IntPtr l) => {
      var mi = new MONINFO(); mi.cbSize = Marshal.SizeOf(typeof(MONINFO));
      GetMonitorInfo(h, ref mi);
      Console.WriteLine("{0} {1},{2} {3}x{4}{5}", mi.szDevice, mi.rcMonitor.Left, mi.rcMonitor.Top,
        mi.rcMonitor.Right - mi.rcMonitor.Left, mi.rcMonitor.Bottom - mi.rcMonitor.Top, (mi.dwFlags & 1) != 0 ? " primary" : "");
      return true; }, IntPtr.Zero);
  }
}
"@
# Per-monitor v2 (-4): every rectangle in physical pixels, none scaled to the primary.
if (-not [Mon]::SetProcessDpiAwarenessContext([IntPtr](-4))) { [Mon]::SetProcessDPIAware() | Out-Null }
[Mon]::List()
