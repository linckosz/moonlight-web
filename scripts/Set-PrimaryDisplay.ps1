# Set-PrimaryDisplay.ps1 -- make one monitor the Windows primary display.
#   .\Set-PrimaryDisplay.ps1 -List
#   .\Set-PrimaryDisplay.ps1 -Device '\\.\DISPLAY5'
# Positions of the other monitors are shifted so the layout is unchanged.
param(
    [switch]$List,
    [string]$Device
)

$src = @"
using System;
using System.Runtime.InteropServices;

public static class PrimDisp
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DEVMODE
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public ushort dmSpecVersion; public ushort dmDriverVersion; public ushort dmSize; public ushort dmDriverExtra; public uint dmFields;
        public int dmPositionX; public int dmPositionY; public uint dmDisplayOrientation; public uint dmDisplayFixedOutput;
        public short dmColor; public short dmDuplex; public short dmYResolution; public short dmTTOption; public short dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public ushort dmLogPixels; public uint dmBitsPerPel; public uint dmPelsWidth; public uint dmPelsHeight; public uint dmDisplayFlags; public uint dmDisplayFrequency;
        public uint dmICMMethod; public uint dmICMIntent; public uint dmMediaType; public uint dmDitherType; public uint dmReserved1; public uint dmReserved2; public uint dmPanningWidth; public uint dmPanningHeight;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DISPLAY_DEVICE
    {
        public int cb;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
        public uint StateFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool EnumDisplayDevices(string lpDevice, uint iDevNum, ref DISPLAY_DEVICE lpDisplayDevice, uint dwFlags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool EnumDisplaySettings(string lpszDeviceName, int iModeNum, ref DEVMODE lpDevMode);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int ChangeDisplaySettingsEx(string lpszDeviceName, ref DEVMODE lpDevMode, IntPtr hwnd, uint dwflags, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int ChangeDisplaySettingsEx(string lpszDeviceName, IntPtr lpDevMode, IntPtr hwnd, uint dwflags, IntPtr lParam);

    public const int ENUM_CURRENT_SETTINGS = -1;
    public const uint DM_POSITION = 0x20;
    public const uint CDS_UPDATEREGISTRY = 0x01;
    public const uint CDS_NORESET = 0x10000000;
    public const uint CDS_SET_PRIMARY = 0x10;
    public const uint ATTACHED = 0x1;
    public const uint PRIMARY = 0x4;
}
"@
if (-not ([System.Management.Automation.PSTypeName]'PrimDisp').Type) { Add-Type -TypeDefinition $src }

$devs = @()
for ($i = 0; $i -lt 16; $i++) {
    $d = New-Object PrimDisp+DISPLAY_DEVICE
    $d.cb = [System.Runtime.InteropServices.Marshal]::SizeOf($d)
    if (-not [PrimDisp]::EnumDisplayDevices($null, $i, [ref]$d, 0)) { break }
    if (($d.StateFlags -band [PrimDisp]::ATTACHED) -eq 0) { continue }
    $m = New-Object PrimDisp+DEVMODE
    $m.dmSize = [System.Runtime.InteropServices.Marshal]::SizeOf($m)
    if (-not [PrimDisp]::EnumDisplaySettings($d.DeviceName, [PrimDisp]::ENUM_CURRENT_SETTINGS, [ref]$m)) { continue }
    $devs += [pscustomobject]@{ Name = $d.DeviceName; Desc = $d.DeviceString; Primary = (($d.StateFlags -band [PrimDisp]::PRIMARY) -ne 0); X = $m.dmPositionX; Y = $m.dmPositionY; W = $m.dmPelsWidth; H = $m.dmPelsHeight; Mode = $m }
}
if ($List -or -not $Device) {
    $devs | ForEach-Object { "{0,-14} primary={1,-5} {2}x{3} at {4},{5}  {6}" -f $_.Name, $_.Primary, $_.W, $_.H, $_.X, $_.Y, $_.Desc }
    if (-not $Device) { exit 0 }
}
$target = $devs | Where-Object { $_.Name -eq $Device } | Select-Object -First 1
if (-not $target) { Write-Error "no attached display named '$Device'"; exit 1 }
if ($target.Primary) { "$Device is already primary"; exit 0 }
$dx = $target.X; $dy = $target.Y
foreach ($d in $devs) {
    $m = $d.Mode
    $m.dmFields = [PrimDisp]::DM_POSITION
    $m.dmPositionX = $d.X - $dx
    $m.dmPositionY = $d.Y - $dy
    $flags = [PrimDisp]::CDS_UPDATEREGISTRY -bor [PrimDisp]::CDS_NORESET
    if ($d.Name -eq $Device) { $flags = $flags -bor [PrimDisp]::CDS_SET_PRIMARY }
    $rc = [PrimDisp]::ChangeDisplaySettingsEx($d.Name, [ref]$m, [IntPtr]::Zero, $flags, [IntPtr]::Zero)
    "{0} -> {1},{2} rc={3}" -f $d.Name, $m.dmPositionX, $m.dmPositionY, $rc
}
$rc = [PrimDisp]::ChangeDisplaySettingsEx($null, [IntPtr]::Zero, [IntPtr]::Zero, 0, [IntPtr]::Zero)
"apply rc=$rc"
