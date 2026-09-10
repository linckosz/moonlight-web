# Set-DisplayRefresh.ps1 -- read or set the refresh rate of a display.
#
#   .\Set-DisplayRefresh.ps1 -List
#   .\Set-DisplayRefresh.ps1 -Device \.\DISPLAY1 -Hz 165
#
# Why this exists: turning Windows HDR on can silently drop the mode. The M27Q
# at 2560x1440 runs 165 Hz in 8-bit SDR and falls back to 60 Hz the moment HDR
# raises the link to 10 bits - and it does NOT come back when HDR is switched
# off again. A campaign that does not put the rate back then measures a screen
# three times slower than the one it measured an hour earlier, and reads it as a
# regression: click-to-photon moves by ~15 ms between 60 Hz and 165 Hz, which is
# larger than every effect the star matrix is looking for.
param(
    [switch] $List,
    [string] $Device = '',
    [int] $Hz = 0
)
Add-Type @"
using System; using System.Runtime.InteropServices;
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
public struct DEVMODEA {
  [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
  public ushort dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
  public uint dmFields;
  public int dmPositionX, dmPositionY; public uint dmDisplayOrientation, dmDisplayFixedOutput;
  public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
  [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
  public ushort dmLogPixels; public uint dmBitsPerPel, dmPelsWidth, dmPelsHeight;
  public uint dmDisplayFlags, dmDisplayFrequency;
  public uint dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
}
public class Disp {
  [DllImport("user32.dll", CharSet = CharSet.Ansi)] public static extern bool EnumDisplaySettingsA(string dev, int mode, ref DEVMODEA dm);
  [DllImport("user32.dll", CharSet = CharSet.Ansi)] public static extern int ChangeDisplaySettingsExA(string dev, ref DEVMODEA dm, IntPtr wnd, uint flags, IntPtr param);
  [DllImport("user32.dll", CharSet = CharSet.Ansi)] public static extern bool EnumDisplayDevicesA(string dev, uint n, ref DISPLAY_DEVICEA d, uint flags);
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
  public struct DISPLAY_DEVICEA {
    public int cb;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
    public uint StateFlags;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
  }
}
"@
# ENUM_CURRENT_SETTINGS
$CURRENT = -1

function Get-Mode([string] $dev) {
    $dm = New-Object DEVMODEA
    $dm.dmSize = [System.Runtime.InteropServices.Marshal]::SizeOf([type][DEVMODEA])
    if ([Disp]::EnumDisplaySettingsA($dev, $CURRENT, [ref]$dm)) { return $dm }
    return $null
}

if ($List -or -not $Device) {
    $i = 0
    while ($true) {
        $d = New-Object Disp+DISPLAY_DEVICEA
        $d.cb = [System.Runtime.InteropServices.Marshal]::SizeOf([type][Disp+DISPLAY_DEVICEA])
        # [NullString]::Value, not $null: PowerShell marshals $null to an empty
        # string here, and EnumDisplayDevices then enumerates nothing at all -
        # a listing that silently prints no display.
        if (-not [Disp]::EnumDisplayDevicesA([NullString]::Value, $i, [ref]$d, 0)) { break }
        # DISPLAY_DEVICE_ATTACHED_TO_DESKTOP
        if ($d.StateFlags -band 1) {
            $m = Get-Mode $d.DeviceName
            if ($m) {
                "{0,-20} {1,5}x{2,-5} {3,4} Hz  {4}" -f $d.DeviceName, $m.dmPelsWidth, $m.dmPelsHeight, $m.dmDisplayFrequency, $d.DeviceString
            }
        }
        $i++
    }
    if (-not $Device) { return }
}

if ($Hz -le 0) { return }
$dm = Get-Mode $Device
if (-not $dm) { throw "no current mode for $Device" }
$was = $dm.dmDisplayFrequency
$dm.dmDisplayFrequency = [uint32]$Hz
# DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY
$dm.dmFields = 0x00080000 -bor 0x00100000 -bor 0x00400000
# CDS_UPDATEREGISTRY (1): keep it across the next boot, like the Settings app.
$rc = [Disp]::ChangeDisplaySettingsExA($Device, [ref]$dm, [IntPtr]::Zero, 1, [IntPtr]::Zero)
$now = (Get-Mode $Device).dmDisplayFrequency
"$Device $was Hz -> $Hz Hz : rc=$rc, now $now Hz"
if ($rc -ne 0) { throw "ChangeDisplaySettingsEx refused the mode (rc=$rc) - the link may not carry it at this depth" }
