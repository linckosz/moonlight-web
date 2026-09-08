# An inert click target for the probe: a small topmost window that never takes
# activation (WS_EX_NOACTIVATE), so the injected clicks land somewhere harmless
# WITHOUT deactivating the kiosk Chrome.
#
# This exists because of a specific, expensive failure: the host pointer was
# parked on the taskbar, an injected click landed on the chevron of the hidden
# icons, the fullscreen Chrome lost activation, its page went visibilityState=
# hidden, rAF froze, and mwLatency.run() never returned. A window that cannot be
# activated cannot do that.
#
#   powershell -NoProfile -File click-target.ps1 -X 1800 -Y 560 -Size 300
#
# Runs until killed. NOTE when killing it from a script: a filter like
# -like '*click-target*' matches the killing shell's own command line and kills
# it instead — exclude $PID.
param([int] $X = 1800, [int] $Y = 560, [int] $Size = 300)
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing @"
using System; using System.Windows.Forms; using System.Drawing; using System.Runtime.InteropServices;
public class NoActivateForm : Form {
  [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  public NoActivateForm(int x, int y, int size) {
    SetProcessDpiAwarenessContext((IntPtr)(-4));
    FormBorderStyle = FormBorderStyle.None; TopMost = true; ShowInTaskbar = false;
    StartPosition = FormStartPosition.Manual; Location = new Point(x, y); Size = new Size(size, size);
    BackColor = Color.FromArgb(40, 40, 40);
    var l = new Label(); l.Text = "probe target"; l.ForeColor = Color.Gray; l.Dock = DockStyle.Fill;
    l.TextAlign = ContentAlignment.MiddleCenter; Controls.Add(l);
  }
  protected override bool ShowWithoutActivation { get { return true; } }
  protected override CreateParams CreateParams { get { var p = base.CreateParams; p.ExStyle |= 0x08000000; return p; } }
}
"@
$f = New-Object NoActivateForm $X, $Y, $Size
[System.Windows.Forms.Application]::Run($f)
