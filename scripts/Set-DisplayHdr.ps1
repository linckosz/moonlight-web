# Set-DisplayHdr.ps1 -- list or switch Windows HDR ("advanced color") per display.
#   .\Set-DisplayHdr.ps1 -List
#   .\Set-DisplayHdr.ps1 -Name M27Q -Enable
#   .\Set-DisplayHdr.ps1 -Name M27Q -Disable
param(
    [switch]$List,
    [string]$Name,
    [switch]$Enable,
    [switch]$Disable
)

$src = @"
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Collections.Generic;

public static class DispHdr
{
    [StructLayout(LayoutKind.Sequential)] public struct LUID { public uint LowPart; public int HighPart; }
    [StructLayout(LayoutKind.Sequential)] public struct RATIONAL { public uint Numerator; public uint Denominator; }
    [StructLayout(LayoutKind.Sequential)] public struct PATH_SOURCE_INFO { public LUID adapterId; public uint id; public uint modeInfoIdx; public uint statusFlags; }
    [StructLayout(LayoutKind.Sequential)] public struct PATH_TARGET_INFO { public LUID adapterId; public uint id; public uint modeInfoIdx; public uint outputTechnology; public uint rotation; public uint scaling; public RATIONAL refreshRate; public uint scanLineOrdering; public int targetAvailable; public uint statusFlags; }
    [StructLayout(LayoutKind.Sequential)] public struct PATH_INFO { public PATH_SOURCE_INFO sourceInfo; public PATH_TARGET_INFO targetInfo; public uint flags; }
    [StructLayout(LayoutKind.Sequential, Size = 64)] public struct MODE_INFO { public uint infoType; public uint id; public LUID adapterId; }

    [StructLayout(LayoutKind.Sequential)] public struct HEADER { public uint type; public uint size; public LUID adapterId; public uint id; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct TARGET_NAME
    {
        public HEADER header; public uint flags; public uint outputTechnology; public ushort edidManufactureId; public ushort edidProductCodeId; public uint connectorInstance;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string monitorFriendlyDeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string monitorDevicePath;
    }
    [StructLayout(LayoutKind.Sequential)] public struct GET_ADVANCED_COLOR { public HEADER header; public uint value; public uint colorEncoding; public uint bitsPerColorChannel; }
    [StructLayout(LayoutKind.Sequential)] public struct SET_ADVANCED_COLOR { public HEADER header; public uint value; }

    [DllImport("user32.dll")] static extern int GetDisplayConfigBufferSizes(uint flags, out uint numPath, out uint numMode);
    [DllImport("user32.dll")] static extern int QueryDisplayConfig(uint flags, ref uint numPath, [Out] PATH_INFO[] paths, ref uint numMode, [Out] MODE_INFO[] modes, IntPtr topology);
    [DllImport("user32.dll")] static extern int DisplayConfigGetDeviceInfo(ref TARGET_NAME p);
    [DllImport("user32.dll")] static extern int DisplayConfigGetDeviceInfo(ref GET_ADVANCED_COLOR p);
    [DllImport("user32.dll")] static extern int DisplayConfigSetDeviceInfo(ref SET_ADVANCED_COLOR p);

    public class Target { public string Name; public LUID Adapter; public uint Id; public bool Supported; public bool Enabled; public uint Bits; }

    public static List<Target> List()
    {
        uint np, nm;
        int rc = GetDisplayConfigBufferSizes(2, out np, out nm);
        if (rc != 0) throw new Exception("GetDisplayConfigBufferSizes " + rc);
        var paths = new PATH_INFO[np]; var modes = new MODE_INFO[nm];
        rc = QueryDisplayConfig(2, ref np, paths, ref nm, modes, IntPtr.Zero);
        if (rc != 0) throw new Exception("QueryDisplayConfig " + rc);
        var list = new List<Target>();
        for (int i = 0; i < np; i++)
        {
            var t = paths[i].targetInfo;
            var tn = new TARGET_NAME();
            tn.header.type = 2; tn.header.size = (uint)Marshal.SizeOf(typeof(TARGET_NAME)); tn.header.adapterId = t.adapterId; tn.header.id = t.id;
            DisplayConfigGetDeviceInfo(ref tn);
            var ac = new GET_ADVANCED_COLOR();
            ac.header.type = 9; ac.header.size = (uint)Marshal.SizeOf(typeof(GET_ADVANCED_COLOR)); ac.header.adapterId = t.adapterId; ac.header.id = t.id;
            DisplayConfigGetDeviceInfo(ref ac);
            list.Add(new Target { Name = tn.monitorFriendlyDeviceName, Adapter = t.adapterId, Id = t.id, Supported = (ac.value & 1) != 0, Enabled = (ac.value & 2) != 0, Bits = ac.bitsPerColorChannel });
        }
        return list;
    }

    public static int Set(Target t, bool enable)
    {
        var s = new SET_ADVANCED_COLOR();
        s.header.type = 10; s.header.size = (uint)Marshal.SizeOf(typeof(SET_ADVANCED_COLOR)); s.header.adapterId = t.Adapter; s.header.id = t.Id;
        s.value = enable ? 1u : 0u;
        return DisplayConfigSetDeviceInfo(ref s);
    }
}
"@
if (-not ([System.Management.Automation.PSTypeName]'DispHdr').Type) { Add-Type -TypeDefinition $src }

$targets = [DispHdr]::List()
if ($List -or -not $Name) {
    $targets | ForEach-Object { "{0,-28} supported={1,-5} enabled={2,-5} bpc={3}" -f $_.Name, $_.Supported, $_.Enabled, $_.Bits }
    if (-not $Name) { exit 0 }
}
$t = $targets | Where-Object { $_.Name -like "*$Name*" } | Select-Object -First 1
if (-not $t) { Write-Error "no display matching '$Name'"; exit 1 }
if ($Enable -or $Disable) {
    $rc = [DispHdr]::Set($t, [bool]$Enable)
    "set {0} HDR={1} rc={2}" -f $t.Name, [bool]$Enable, $rc
    Start-Sleep -Milliseconds 800
    [DispHdr]::List() | Where-Object { $_.Name -eq $t.Name } | ForEach-Object { "{0} now enabled={1} bpc={2}" -f $_.Name, $_.Enabled, $_.Bits }
}
