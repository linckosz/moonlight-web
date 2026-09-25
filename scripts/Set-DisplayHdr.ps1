# Set-DisplayHdr.ps1 -- list or switch Windows HDR ("advanced color") per display.
#   .\Set-DisplayHdr.ps1 -List
#   .\Set-DisplayHdr.ps1 -Name M27Q -Enable
#   .\Set-DisplayHdr.ps1 -Device \\.\DISPLAY5 -Disable
#
# -Device names the screen by its GDI name, which is unique; -Name matches the
# monitor's model name, which is not when a desk has several of one model (the
# three M27Q of DualRTX) - it then takes the first, and that may be the wrong one.
param(
    [switch]$List,
    [string]$Name,
    [string]$Device,
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
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct SOURCE_NAME { public HEADER header; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string viewGdiDeviceName; }
    [StructLayout(LayoutKind.Sequential)] public struct GET_ADVANCED_COLOR { public HEADER header; public uint value; public uint colorEncoding; public uint bitsPerColorChannel; }
    [StructLayout(LayoutKind.Sequential)] public struct SET_ADVANCED_COLOR { public HEADER header; public uint value; }

    [DllImport("user32.dll")] static extern int GetDisplayConfigBufferSizes(uint flags, out uint numPath, out uint numMode);
    [DllImport("user32.dll")] static extern int QueryDisplayConfig(uint flags, ref uint numPath, [Out] PATH_INFO[] paths, ref uint numMode, [Out] MODE_INFO[] modes, IntPtr topology);
    [DllImport("user32.dll")] static extern int DisplayConfigGetDeviceInfo(ref TARGET_NAME p);
    [DllImport("user32.dll")] static extern int DisplayConfigGetDeviceInfo(ref SOURCE_NAME p);
    [DllImport("user32.dll")] static extern int DisplayConfigGetDeviceInfo(ref GET_ADVANCED_COLOR p);
    [DllImport("user32.dll")] static extern int DisplayConfigSetDeviceInfo(ref SET_ADVANCED_COLOR p);

    public class Target { public string Name; public string Gdi; public LUID Adapter; public LUID Render; public uint Id; public bool Supported; public bool Enabled; public uint Bits; }

    // The GPU that renders each screen, by GDI name. For a physical screen it
    // is the adapter of its path; for an indirect (virtual) display the path
    // names the virtual adapter, and only DXGI - which lists the output under
    // the GPU that composes it - says which GPU a client on it decodes with.
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] public struct DXGI_ADAPTER_DESC { [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Description; public uint VendorId, DeviceId, SubSysId, Revision; public UIntPtr Dedicated, DedicatedSystem, SharedSystem; public LUID Luid; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] public struct DXGI_OUTPUT_DESC { [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName; public int Left, Top, Right, Bottom; public int Attached; public int Rotation; public IntPtr Monitor; }
    [ComImport, Guid("ae02eedb-c735-4690-8d52-5a8dc20213aa"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IDXGIOutput { void SetPrivateData(); void SetPrivateDataInterface(); void GetPrivateData(); void GetParent(); [PreserveSig] int GetDesc(out DXGI_OUTPUT_DESC d); }
    [ComImport, Guid("2411e7e1-12ac-4ccf-bd14-9798e8534dc0"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IDXGIAdapter { void SetPrivateData(); void SetPrivateDataInterface(); void GetPrivateData(); void GetParent(); [PreserveSig] int EnumOutputs(uint i, out IDXGIOutput o); [PreserveSig] int GetDesc(out DXGI_ADAPTER_DESC d); }
    [ComImport, Guid("770aae78-f26f-4dba-a829-253c83d1b387"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IDXGIFactory1 { void SetPrivateData(); void SetPrivateDataInterface(); void GetPrivateData(); void GetParent(); [PreserveSig] int EnumAdapters(uint i, out IDXGIAdapter a); }
    [DllImport("dxgi.dll")] static extern int CreateDXGIFactory1(ref Guid riid, out IDXGIFactory1 f);

    public static Dictionary<string, LUID> RenderAdapters()
    {
        var map = new Dictionary<string, LUID>(StringComparer.OrdinalIgnoreCase);
        var iid = typeof(IDXGIFactory1).GUID; IDXGIFactory1 f;
        if (CreateDXGIFactory1(ref iid, out f) != 0) return map;
        IDXGIAdapter a;
        for (uint i = 0; f.EnumAdapters(i, out a) == 0; i++)
        {
            DXGI_ADAPTER_DESC ad; a.GetDesc(out ad);
            IDXGIOutput o;
            for (uint j = 0; a.EnumOutputs(j, out o) == 0; j++) { DXGI_OUTPUT_DESC od; o.GetDesc(out od); map[od.DeviceName] = ad.Luid; }
        }
        return map;
    }

    public static List<Target> List()
    {
        uint np, nm;
        int rc = GetDisplayConfigBufferSizes(2, out np, out nm);
        if (rc != 0) throw new Exception("GetDisplayConfigBufferSizes " + rc);
        var paths = new PATH_INFO[np]; var modes = new MODE_INFO[nm];
        rc = QueryDisplayConfig(2, ref np, paths, ref nm, modes, IntPtr.Zero);
        if (rc != 0) throw new Exception("QueryDisplayConfig " + rc);
        var list = new List<Target>();
        var render = RenderAdapters();
        for (int i = 0; i < np; i++)
        {
            var t = paths[i].targetInfo;
            var tn = new TARGET_NAME();
            tn.header.type = 2; tn.header.size = (uint)Marshal.SizeOf(typeof(TARGET_NAME)); tn.header.adapterId = t.adapterId; tn.header.id = t.id;
            DisplayConfigGetDeviceInfo(ref tn);
            var ac = new GET_ADVANCED_COLOR();
            ac.header.type = 9; ac.header.size = (uint)Marshal.SizeOf(typeof(GET_ADVANCED_COLOR)); ac.header.adapterId = t.adapterId; ac.header.id = t.id;
            DisplayConfigGetDeviceInfo(ref ac);
            var s = paths[i].sourceInfo;
            var sn = new SOURCE_NAME();
            sn.header.type = 1; sn.header.size = (uint)Marshal.SizeOf(typeof(SOURCE_NAME)); sn.header.adapterId = s.adapterId; sn.header.id = s.id;
            DisplayConfigGetDeviceInfo(ref sn);
            LUID r;
            if (!render.TryGetValue(sn.viewGdiDeviceName ?? "", out r)) r = t.adapterId;
            list.Add(new Target { Name = tn.monitorFriendlyDeviceName, Gdi = sn.viewGdiDeviceName, Adapter = t.adapterId, Render = r, Id = t.id, Supported = (ac.value & 1) != 0, Enabled = (ac.value & 2) != 0, Bits = ac.bitsPerColorChannel });
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
if ($List -or (-not $Name -and -not $Device)) {
    # adapter= addresses the screen (it is what -Enable/-Disable act on);
    # render= is the GPU that draws it - the one to pin a client on.
    $targets | ForEach-Object { "{0,-14} {1,-28} supported={2,-5} enabled={3,-5} bpc={4} adapter={5},{6} render={7},{8}" -f $_.Gdi, $_.Name, $_.Supported, $_.Enabled, $_.Bits, $_.Adapter.HighPart, $_.Adapter.LowPart, $_.Render.HighPart, $_.Render.LowPart }
    if (-not $Name -and -not $Device) { exit 0 }
}
$t = if ($Device) { $targets | Where-Object { $_.Gdi -eq $Device } | Select-Object -First 1 }
     else { $targets | Where-Object { $_.Name -like "*$Name*" } | Select-Object -First 1 }
if (-not $t) { Write-Error "no display matching '$Device$Name'"; exit 1 }
if ($Enable -or $Disable) {
    $rc = [DispHdr]::Set($t, [bool]$Enable)
    "set {0} ({1}) HDR={2} rc={3}" -f $t.Gdi, $t.Name, [bool]$Enable, $rc
    Start-Sleep -Milliseconds 800
    [DispHdr]::List() | Where-Object { $_.Gdi -eq $t.Gdi } | ForEach-Object { "{0} ({1}) now enabled={2} bpc={3}" -f $_.Gdi, $_.Name, $_.Enabled, $_.Bits }
    if ($rc -ne 0) { exit 1 }
}
