# ============================================================================
# The FOURTH instrument: does a native stream follow the host's screen?
#
#   .\display-follow.ps1 [-AppUrl https://127.0.0.1:18443/ -ApiUrl http://127.0.0.1:18080]
#                        [-Device \\.\DISPLAY30] [-NoClientFlip] [-KeepKiosks] [-Share]
#
# A native host knows its own display, so two things are not settings at all:
#
#   Shape  in aspect Auto the stream takes the display's real ratio, and a
#          mode change mid-stream (a game switching to 4:3) rebuilds the
#          encoder at the new shape without a relaunch - ONE new decoder on
#          the client, the next keyframe, and nothing else.
#   HDR    the checkbox is ignored. The stream is HDR exactly when the host's
#          display is in HDR AND the client's screen can show it; otherwise
#          SDR, converted on the host if its display is HDR. Either side
#          changing mid-stream moves the stream to the new answer through the
#          seamless relaunch.
#
# Neither is a speed, and both fail silently: a 4:3 game in a 16:9 frame is
# only black bars, an HDR desktop streamed as SDR to an HDR screen is only a
# duller picture, and three decoders created in a row where one was enough is
# nothing visible at all. So this script CAUSES each change - it switches the
# host display's mode and HDR itself - and reads both ends of the stream back:
# the host says what it encodes (the worker log), the page says what it
# decoded and whether it relaunched (its console).
#
# It changes real display settings, so it chooses its screen carefully:
#
#   - The captured screen defaults to a VIRTUAL display (a VDD, an indirect
#     display driver): it accepts HDR and 4:3 modes and nobody sees it flicker.
#     A physical screen is only taken with -Device, never guessed.
#   - The client's screen is flipped out of HDR and back (-NoClientFlip to
#     skip). That one IS physical, and on some monitors HDR drops the refresh
#     rate and does not raise it again, so its mode is put back afterwards
#     along with its HDR state.
#   - Everything touched is restored in a finally, however the script leaves.
#
# The client is a kiosk Chrome driven over CDP like the browser half's, and
# the captured screen shows content/scroll.html so every frame is new.
#
# -Share plays the story again with the stream SHARED: a guest joins through a
# share link in a second kiosk beside the owner's. A share pins the owner's
# stream against the client's own changes, never against the host's:
#
#   Shape  the guest's stream follows the display as well (its own worker, its
#          own encoder rebuilt at the new shape).
#   HDR    the host display entering HDR still moves the owner's stream, and
#          the guest keeps streaming through that relaunch. The client's screen
#          leaving HDR is held back while the share is up, and taken up once
#          the share is over.
# ============================================================================
param(
    [string] $ResultsDir = '',
    [string] $AppUrl = 'https://127.0.0.1:8443/',
    [string] $ApiUrl = 'http://127.0.0.1:8080',
    # GDI name of the captured display (\\.\DISPLAYn). Empty: a virtual one.
    [string] $Device = '',
    [string] $Tile = '',
    [string] $LogDir = '',
    [int]    $DebugPort = 9333,
    [string] $ClientAdapterLuid = '',
    [int]    $TimeoutSec = 30,
    [switch] $NoClientFlip,
    [switch] $KeepKiosks,
    [switch] $Share,
    [int]    $GuestDebugPort = 9334
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $ResultsDir) { $ResultsDir = Join-Path $PSScriptRoot 'results' }
if (-not (Test-Path $ResultsDir)) { New-Item -ItemType Directory -Path $ResultsDir | Out-Null }
Set-Location $PSScriptRoot

function Get-Prop {
    param($Object, [string] $Name, $Default = $null)
    if ($null -eq $Object) { return $Default }
    if ($Object.PSObject.Properties.Name -contains $Name) { return $Object.$Name }
    return $Default
}

function Cdp {
    param([Parameter(ValueFromRemainingArguments = $true)] $Args)
    # A page that is not up yet makes cdp.py print a traceback: an answer to
    # poll again, not a reason to stop the script.
    $ErrorActionPreference = 'Continue'
    & python "$PSScriptRoot\cdp.py" --port $DebugPort @Args 2>&1 | Out-String
}
# The same against another kiosk: the guest's, with -Share.
function CdpAt {
    param([int] $Port, [Parameter(ValueFromRemainingArguments = $true)] $Rest)
    $ErrorActionPreference = 'Continue'
    & python "$PSScriptRoot\cdp.py" --port $Port @Rest 2>&1 | Out-String
}

# -- Screens: modes and HDR by GDI name --------------------------------------
# DisplayConfig names a screen two ways that have to be joined: the SOURCE
# carries the GDI name (\\.\DISPLAY30) that ChangeDisplaySettingsEx takes, the
# TARGET carries the monitor's friendly name and its advanced-colour state.
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class BenchScreens {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DEVMODE {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
        public int dmFields;
        public int dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels;
        public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
        public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2;
        public int dmPanningWidth, dmPanningHeight;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern bool EnumDisplaySettingsW(string dev, int mode, ref DEVMODE dm);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int ChangeDisplaySettingsExW(string dev, ref DEVMODE dm, IntPtr hwnd, int flags, IntPtr lp);

    [StructLayout(LayoutKind.Sequential)] public struct LUID { public uint Low; public int High; }
    [StructLayout(LayoutKind.Sequential)] struct PATH_SOURCE { public LUID adapterId; public uint id; public uint modeInfoIdx; public uint statusFlags; }
    [StructLayout(LayoutKind.Sequential)] struct RATIONAL { public uint n; public uint d; }
    [StructLayout(LayoutKind.Sequential)] struct PATH_TARGET { public LUID adapterId; public uint id; public uint modeInfoIdx; public uint outputTechnology; public uint rotation; public uint scaling; public RATIONAL refresh; public uint scanLine; public int targetAvailable; public uint statusFlags; }
    [StructLayout(LayoutKind.Sequential)] struct PATH_INFO { public PATH_SOURCE sourceInfo; public PATH_TARGET targetInfo; public uint flags; }
    [StructLayout(LayoutKind.Sequential, Size = 64)] struct MODE_INFO { public uint infoType; public uint id; public LUID adapterId; }
    [StructLayout(LayoutKind.Sequential)] struct HEADER { public uint type; public uint size; public LUID adapterId; public uint id; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct SOURCE_NAME { public HEADER header; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string gdiName; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct TARGET_NAME {
        public HEADER header; public uint flags; public uint outputTechnology; public ushort edidManufactureId; public ushort edidProductCodeId; public uint connectorInstance;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string friendlyName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string devicePath;
    }
    [StructLayout(LayoutKind.Sequential)] struct ADV_COLOR { public HEADER header; public uint value; public uint colorEncoding; public uint bitsPerColorChannel; }
    [StructLayout(LayoutKind.Sequential)] struct SET_ADV_COLOR { public HEADER header; public uint enable; }
    [DllImport("user32.dll")] static extern int GetDisplayConfigBufferSizes(uint flags, out uint paths, out uint modes);
    [DllImport("user32.dll")] static extern int QueryDisplayConfig(uint flags, ref uint paths, [Out] PATH_INFO[] p, ref uint modes, [Out] MODE_INFO[] m, IntPtr topo);
    [DllImport("user32.dll", EntryPoint = "DisplayConfigGetDeviceInfo")] static extern int GetSourceName(ref SOURCE_NAME r);
    [DllImport("user32.dll", EntryPoint = "DisplayConfigGetDeviceInfo")] static extern int GetTargetName(ref TARGET_NAME r);
    [DllImport("user32.dll", EntryPoint = "DisplayConfigGetDeviceInfo")] static extern int GetAdvColor(ref ADV_COLOR r);
    [DllImport("user32.dll", EntryPoint = "DisplayConfigSetDeviceInfo")] static extern int SetAdvColor(ref SET_ADV_COLOR r);

    public class Screen {
        public string Gdi; public string Name; public int X, Y, W, H, Hz;
        public bool HdrSupported, HdrOn; public LUID Adapter; public uint TargetId;
    }

    public static List<Screen> List() {
        uint np, nm;
        GetDisplayConfigBufferSizes(2, out np, out nm);
        var paths = new PATH_INFO[np]; var modes = new MODE_INFO[nm];
        if (QueryDisplayConfig(2, ref np, paths, ref nm, modes, IntPtr.Zero) != 0) throw new Exception("QueryDisplayConfig failed");
        var list = new List<Screen>();
        for (int i = 0; i < np; i++) {
            var sn = new SOURCE_NAME();
            sn.header.type = 1; sn.header.size = (uint)Marshal.SizeOf(typeof(SOURCE_NAME));
            sn.header.adapterId = paths[i].sourceInfo.adapterId; sn.header.id = paths[i].sourceInfo.id;
            GetSourceName(ref sn);
            var tn = new TARGET_NAME();
            tn.header.type = 2; tn.header.size = (uint)Marshal.SizeOf(typeof(TARGET_NAME));
            tn.header.adapterId = paths[i].targetInfo.adapterId; tn.header.id = paths[i].targetInfo.id;
            GetTargetName(ref tn);
            var ac = new ADV_COLOR();
            ac.header.type = 9; ac.header.size = (uint)Marshal.SizeOf(typeof(ADV_COLOR));
            ac.header.adapterId = paths[i].targetInfo.adapterId; ac.header.id = paths[i].targetInfo.id;
            GetAdvColor(ref ac);
            var dm = new DEVMODE(); dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
            EnumDisplaySettingsW(sn.gdiName, -1, ref dm);
            list.Add(new Screen {
                Gdi = sn.gdiName, Name = tn.friendlyName, X = dm.dmPositionX, Y = dm.dmPositionY,
                W = dm.dmPelsWidth, H = dm.dmPelsHeight, Hz = dm.dmDisplayFrequency,
                HdrSupported = (ac.value & 1) != 0, HdrOn = (ac.value & 2) != 0,
                Adapter = paths[i].targetInfo.adapterId, TargetId = paths[i].targetInfo.id });
        }
        return list;
    }

    public static List<int[]> Modes(string gdi) {
        var seen = new HashSet<long>(); var list = new List<int[]>();
        var dm = new DEVMODE(); dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
        for (int i = 0; EnumDisplaySettingsW(gdi, i, ref dm); i++) {
            if (seen.Add(((long)dm.dmPelsWidth << 32) | (uint)dm.dmPelsHeight))
                list.Add(new int[] { dm.dmPelsWidth, dm.dmPelsHeight });
        }
        return list;
    }

    public static int SetMode(string gdi, int w, int h, int hz) {
        var dm = new DEVMODE(); dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
        EnumDisplaySettingsW(gdi, -1, ref dm);
        dm.dmPelsWidth = w; dm.dmPelsHeight = h; dm.dmFields = 0x80000 | 0x100000;
        if (hz > 0) { dm.dmDisplayFrequency = hz; dm.dmFields |= 0x400000; }
        return ChangeDisplaySettingsExW(gdi, ref dm, IntPtr.Zero, 0, IntPtr.Zero);
    }

    public static int SetHdr(string gdi, bool on) {
        foreach (var s in List()) {
            if (s.Gdi != gdi) continue;
            var r = new SET_ADV_COLOR();
            r.header.type = 10; r.header.size = (uint)Marshal.SizeOf(typeof(SET_ADV_COLOR));
            r.header.adapterId = s.Adapter; r.header.id = s.TargetId; r.enable = on ? 1u : 0u;
            return SetAdvColor(ref r);
        }
        return -1;
    }
}
"@

function Get-Screen([string] $Gdi) {
    [BenchScreens]::List() | Where-Object { $_.Gdi -eq $Gdi } | Select-Object -First 1
}

function Set-ScreenHdr([string] $Gdi, [bool] $On) {
    $rc = [BenchScreens]::SetHdr($Gdi, $On)
    if ($rc -ne 0) { throw "could not turn HDR $(if ($On) {'on'} else {'off'}) on $Gdi (rc=$rc)" }
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 250
        if ((Get-Screen $Gdi).HdrOn -eq $On) { return }
    }
    throw "HDR on $Gdi never reached $On"
}

function Set-ScreenMode([string] $Gdi, [int] $W, [int] $H, [int] $Hz = 0) {
    $rc = [BenchScreens]::SetMode($Gdi, $W, $H, $Hz)
    if ($rc -ne 0) { throw "could not set $Gdi to ${W}x${H} (ChangeDisplaySettingsEx rc=$rc)" }
}

# -- Which screen is captured, and which one the client sits on ---------------
$status = Invoke-RestMethod -Uri "$ApiUrl/api/native/status" -Method Get -TimeoutSec 10
if (-not (Get-Prop $status 'available' $false)) {
    throw "no native host behind $ApiUrl - this instrument is about the native host only"
}
$screens = @([BenchScreens]::List())
# A native display is named after its monitor in `detail` ("VDD by MTT - 2560x1440",
# with an em dash), which is how it is joined to a GDI name. Everything up to the
# first non-ASCII character: Windows PowerShell decodes the reply as Latin-1, so
# the dash arrives as three characters and a split on it never matches.
function Get-NativeDisplay($Screen) {
    @($status.displays) | Where-Object {
        (Get-Prop $_ 'detail' '') -match '^(.*?)\s+[^\x00-\x7F]' -and $Matches[1] -eq $Screen.Name
    } | Select-Object -First 1
}

# Without -Device the captured screen is the product's own "MoonlightWeb
# Virtual Display". It does not exist between streams: a stream on its tile
# turns it on, and it goes off 4 s after the last one ends. So it is found the
# way cadence.py finds it - launch its tile, and take the screen that was not
# there before - and it is kept alive from then on (see Reset-Page).
$script:vdMode = -not $Device
$script:vdName = ''
$script:before = @()
$cap = $null
if ($script:vdMode) {
    $vd = $null
    try { $vd = Invoke-RestMethod -Uri "$ApiUrl/api/native/virtual-display" -Method Get -TimeoutSec 10 } catch { }
    if (-not $vd -or -not (Get-Prop $vd 'installed' $false)) {
        throw ("no MoonlightWeb Virtual Display on this host to change modes on. This script switches the " +
               "captured screen's mode and HDR; it will not guess a physical monitor for that. Pass " +
               "-Device \\.\DISPLAYn to name one on purpose (it will flicker).")
    }
    if (-not $Tile) { $Tile = Get-Prop $vd 'name' 'MoonlightWeb Virtual Display' }
    $script:before = @($screens | ForEach-Object { $_.Gdi })
} else {
    $cap = Get-Screen $Device
    if (-not $cap) { throw "no active display named $Device" }
    $native = Get-NativeDisplay $cap
    if (-not $Tile) {
        $Tile = Get-Prop $native 'label' ''
        if (-not $Tile) { throw "cannot find the native tile for $Device ($($cap.Name)) - pass -Tile" }
    }
}

# The virtual display, once a stream has it up: the screen that appeared,
# then the same monitor by name (a display turned off and on again may come
# back under another GDI name). Updates $Device and $cap.
function Update-VirtualScreen {
    if (-not $script:vdMode) { return $true }
    $now = @([BenchScreens]::List())
    # By name, but never a screen that was there before the launch: the desk's
    # own "VDD by MTT" (DualRTX, 25/09/2026) carries the very same name.
    $found = @(if ($script:vdName) { $now | Where-Object { $_.Name -eq $script:vdName -and $script:before -notcontains $_.Gdi } }
               else { $now | Where-Object { $script:before -notcontains $_.Gdi } })
    if ($found.Count -ne 1) { return $false }
    $script:vdName = $found[0].Name
    $script:Device = $found[0].Gdi
    $script:cap = $found[0]
    return $true
}

$clientCandidates = @($screens | Where-Object { $_.Gdi -ne $Device } | Sort-Object { $_.W * $_.H } -Descending)
if ($clientCandidates.Count -eq 0) {
    throw "the captured screen is the only one: the client kiosk would film itself"
}
$client = $clientCandidates[0]
# The client decodes on -ClientAdapterLuid: its window goes on a screen that
# adapter drives, or Chrome draws on one GPU and scans out on another.
if ($ClientAdapterLuid -match '^(-?\d+),(\d+)$') {
    $luid = "$($Matches[1]),$($Matches[2])"
    # The GPU that renders each screen (render=), not the adapter of its path:
    # a virtual display's path names the virtual adapter, never the GPU behind it.
    $render = @{}
    & powershell -NoProfile -File (Join-Path (Split-Path $PSScriptRoot -Parent) 'Set-DisplayHdr.ps1') -List |
        ForEach-Object { if ($_ -match '^(\S+)\s.*render=(-?\d+,\d+)') { $render[$Matches[1]] = $Matches[2] } }
    $onAdapter = @($clientCandidates | Where-Object { $render[$_.Gdi] -eq $luid })
    if ($onAdapter.Count -gt 0) { $client = $onAdapter[0] }
    else { Write-Warning "no screen on adapter ${ClientAdapterLuid}: the client kiosk goes on $($client.Gdi)" }
}

# The alternative shape: the mode furthest from the base ratio that is still a
# real desktop, preferring 4:3 - the shape a game actually switches to - and,
# among equals, the size nearest the base one. The virtual display lists 16:9
# sizes only, besides the size it was made at: it is launched at a 4:3 custom
# size (below), and its other shape is then a 16:9 one.
function Get-AltMode {
    $baseRatio = $cap.W / $cap.H
    $target = if ([Math]::Abs($baseRatio - 4 / 3) -lt 0.05) { 16 / 9 } else { 4 / 3 }
    @([BenchScreens]::Modes($Device) | Where-Object { $_[0] -ge 800 -and $_[1] -ge 600 } |
        Where-Object { [Math]::Abs(($_[0] / $_[1]) - $baseRatio) -gt 0.05 } |
        Sort-Object { [Math]::Abs(($_[0] / $_[1]) - $target) }, { [Math]::Abs($_[0] * $_[1] - $cap.W * $cap.H) }) |
        Select-Object -First 1
}
if (-not $script:vdMode) {
    $alt = Get-AltMode
    if (-not $alt) { throw "$Device offers no mode of another shape than $($cap.W)x$($cap.H)" }
    Write-Host "captured     : $Device ($($cap.Name)) $($cap.W)x$($cap.H)@$($cap.Hz) HDR=$($cap.HdrOn) -> tile '$Tile'"
    Write-Host "other shape  : $($alt[0])x$($alt[1])"
} else {
    Write-Host "captured     : the virtual display, made by a stream on tile '$Tile'"
}
Write-Host "client screen: $($client.Gdi) ($($client.Name)) $($client.W)x$($client.H)@$($client.Hz) HDR=$($client.HdrOn)"

if (-not $LogDir) {
    # A dev instance writes to its own profile; read the one whose WORKER log
    # moved last. Any log would do not: the virtual display's elevated helper
    # writes moonlightweb-vdisplay.log into the production profile whatever
    # instance asked it, and on 25/09/2026 that sent the script to a folder
    # with none of the stream's lines in it — every change read "0x0".
    $roots = @('MoonlightWeb-dev', 'MoonlightWebDev', 'MoonlightWeb') |
        ForEach-Object { Join-Path $env:APPDATA "MoonlightWeb\$_\logs" } | Where-Object { Test-Path $_ }
    $LogDir = $roots | Sort-Object {
        $newest = Get-ChildItem $_ -Filter 'moonlightweb-worker-*.log' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($newest) { $newest.LastWriteTime } else { [datetime]::MinValue }
    } -Descending | Select-Object -First 1
}
Write-Host "worker logs  : $LogDir"

# -- Both ends of the stream, read back ---------------------------------------
# The HOST: every worker log that grew since the mark. A seamless relaunch is a
# second worker with its own file, so one file is never enough.
#
# Sizes are read through a handle, never from the directory listing: NTFS
# updates a listing's size lazily while the writer keeps the file open, and a
# worker keeps its log open for its whole life. Read from the listing, a
# worker's log never seemed to grow and every change read "host encodes 0x0".
function Get-LiveLength([string] $Path) {
    try {
        $fs = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
        try { return $fs.Length } finally { $fs.Dispose() }
    } catch { return 0 }
}
#
# Only the logs this run can have written to: a profile keeps every worker log
# it ever had (1,830 on DualRTX, 25/09/2026), and opening them all on every
# poll made one poll outlast the change it was waiting for. Written to during
# the run, or belonging to a worker alive right now — the second because the
# listing's dates are as lazy as its sizes while a worker holds its log open,
# and because a pid comes back: worker 50044 appended to a log of 02/09.
$script:logSince = (Get-Date).AddMinutes(-2)
function Get-WorkerLogs {
    $live = @(Get-CimInstance Win32_Process -Filter "Name='MoonlightWeb.exe' OR Name='MoonlightWebDev.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like '*--stream-worker*' } |
        ForEach-Object { "moonlightweb-worker-$($_.ProcessId).log" })
    Get-ChildItem (Join-Path $LogDir 'moonlightweb-worker-*.log') -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -gt $script:logSince -or $live -contains $_.Name }
}
# The mark also carries its time: a log that comes back under a reused pid
# holds another day's sessions before the offset of a file first seen now.
function Get-LogMark {
    $m = @{ '__since' = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff') }
    Get-WorkerLogs | ForEach-Object { $m[$_.FullName] = Get-LiveLength $_.FullName }
    return $m
}
# With -Share the guest's worker writes a log of its own: kept out of the
# owner's lines, and read alone when -Only names it.
$script:GuestLog = ''
function Get-HostLines($Mark, [string] $Only = '') {
    $lines = @()
    foreach ($f in Get-WorkerLogs) {
        if ($Only) { if ($f.FullName -ne $Only) { continue } }
        elseif ($script:GuestLog -and $f.FullName -eq $script:GuestLog) { continue }
        $from = if ($Mark.ContainsKey($f.FullName)) { $Mark[$f.FullName] } else { 0 }
        $fs = [System.IO.File]::Open($f.FullName, 'Open', 'Read', 'ReadWrite')
        if ($fs.Length -le $from) { $fs.Dispose(); continue }
        try {
            $fs.Seek($from, 'Begin') | Out-Null
            $text = (New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::UTF8)).ReadToEnd()
        } finally { $fs.Dispose() }
        $since = if ($Mark.ContainsKey('__since')) { $Mark['__since'] } else { '' }
        $lines += @($text -split "`r?`n" | Where-Object {
            $_ -match '\[native\] (session:|display format:|session ended|AcquireNextFrame failed)' -and
            (-not $since -or ($_.Length -gt 24 -and $_.Substring(1, 23) -ge $since))
        })
    }
    # Two workers overlap during a transition: order by their own timestamps.
    return @($lines | Sort-Object)
}
# What the host is encoding NOW, from those lines: the last word wins.
function Get-HostState($Lines) {
    $s = [ordered]@{ w = 0; h = 0; hdr = $null; displayW = 0; displayH = 0; displayHdr = $null; sessions = 0 }
    foreach ($l in $Lines) {
        if ($l -match '\[native\] session: display \d+ (\d+)x(\d+)@\d+ (.*)$') {
            $s.w = [int]$Matches[1]; $s.h = [int]$Matches[2]; $s.hdr = ($Matches[3] -match '\(HDR\)')
            $s.sessions++
        } elseif ($l -match 'display format: (\d+)x(\d+) (HDR|SDR), streaming (\d+)x(\d+) (HDR|SDR)') {
            $s.displayW = [int]$Matches[1]; $s.displayH = [int]$Matches[2]; $s.displayHdr = $Matches[3] -eq 'HDR'
            $s.w = [int]$Matches[4]; $s.h = [int]$Matches[5]; $s.hdr = $Matches[6] -eq 'HDR'
        }
    }
    return [pscustomobject]$s
}

# The PAGE: its console, kept in the page from before the launch so nothing a
# transition logs is missed between two polls. The decoder lines are there too
# when decoding runs on the main thread, which is the default renderer.
$hookJs = Join-Path $ResultsDir 'display-follow-hook.js'
@'
(() => {
  if (window.__mwFollow) return 'kept';
  window.__mwFollow = [];
  for (const k of ['log', 'info', 'warn', 'error']) {
    const o = console[k].bind(console);
    console[k] = (...a) => {
      try { window.__mwFollow.push({ t: Date.now(), s: a.map(x => typeof x === 'string' ? x : (x && x.message) || String(x)).join(' ') }); } catch (e) {}
      o(...a);
    };
  }
  // The client's own screen change, as the page's media query reports it: a
  // stream that did not move after a flip is either the app or the browser,
  // and this line says which.
  const mq = matchMedia('(dynamic-range: high)');
  mq.addEventListener('change', () => console.log('[bench] dynamic-range change: high=' + mq.matches));
  return 'armed';
})()
'@ | Set-Content -Path $hookJs -Encoding ASCII

function Get-PageNow { [int64](Cdp eval 'Date.now()').Trim() }
function Get-PageLines([int64] $Since) {
    $raw = (Cdp eval "JSON.stringify((window.__mwFollow || []).filter(e => e.t >= $Since).map(e => e.s))").Trim()
    # Parenthesised: Windows PowerShell hands a JSON array down the pipeline as
    # ONE object, and every filter after it would match the whole console.
    try { return @(($raw | ConvertFrom-Json)) } catch { return @() }
}
# What the page decodes now: the largest picture on it. A canvas's backing
# size, or a <video>'s own size - an HEVC HDR stream on an HDR screen is drawn
# by a <video> sink, and a canvas-only look reads it as no picture at all.
function Get-View([int] $Port = $DebugPort) {
    $raw = (CdpAt $Port eval "JSON.stringify((() => { const p = [...document.querySelectorAll('canvas')].filter(e => e.getBoundingClientRect().width > 0).map(e => ({ w: e.width, h: e.height, sink: 'canvas' })).concat([...document.querySelectorAll('video')].filter(e => e.videoWidth > 0).map(e => ({ w: e.videoWidth, h: e.videoHeight, sink: 'video' }))).sort((a, b) => b.w * b.h - a.w * a.h)[0] || { w: 0, h: 0, sink: 'none' }; p.screenHdr = matchMedia('(dynamic-range: high)').matches; return p; })())").Trim()
    try { return $raw | ConvertFrom-Json } catch { return [pscustomobject]@{ w = 0; h = 0; sink = 'none'; screenHdr = $false } }
}

function Wait-For([scriptblock] $Condition, [int] $Seconds = $TimeoutSec) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
        if (& $Condition) { return [int]$sw.ElapsedMilliseconds }
        Start-Sleep -Milliseconds 500
    }
    return -1
}

function Test-Ratio([int] $W, [int] $H, [int] $RefW, [int] $RefH) {
    if ($W -le 0 -or $H -le 0) { return $false }
    # Encoders round to even sizes, and a 16:10 display at 1080 lines is 1728 wide.
    return [Math]::Abs(($W / $H) - ($RefW / $RefH)) -lt 0.02
}

# Leave the stream by reloading the page, not through its Stop button: after
# a seamless transition, in fullscreen, the button is not on the page, the
# stream silently carried on, and the next "launch" measured a relaunch the
# old stream had made on its own. The next launch takes the host over anyway.
#
# The virtual display goes off 4 s after the last stream on it ends, and the
# next one comes up in SDR whatever it was (the product forces it at every
# activation). So in that mode the reload does not idle: the next launch has
# to land inside those 4 s, and the caller launches straight after.
function Reset-Page {
    Cdp exitfs | Out-Null
    Cdp call Page.reload | Out-Null
    if (-not $script:vdMode) { Start-Sleep -Seconds 2 }
    $ready = Wait-For { (Cdp eval 'document.body ? document.body.innerText : 0') -match [regex]::Escape($Tile) } 60
    if ($ready -lt 0) { throw "after a reload the tile '$Tile' never came back at $AppUrl" }
    Cdp evalfile $hookJs | Out-Null
    if ((Get-View).w -gt 0) { throw 'a picture is still on the page after a reload' }
}

function Start-Stream {
    Cdp launch $Tile | Out-Null
    # The self-stream warning can come up late (a narrower window took longer
    # than the three seconds once waited here): answered whenever it shows.
    $ms = Wait-For {
        if ((Cdp eval "!!document.querySelector('.self-stream-go')") -match 'true') {
            Cdp launch 'Stream anyway' | Out-Null
        }
        (Get-View).w -gt 0
    } 40
    if ($ms -lt 0) { throw "the stream on '$Tile' never showed a picture" }
    # Shared, the owner's kiosk holds half the screen and the guest's the other
    # half: fullscreen would cover the guest, and Chrome stops painting a
    # window nothing of which is visible.
    if (-not $Share) { Cdp fullscreen | Out-Null }
    Start-Sleep -Seconds 3
    # Relaunched after a stream died, the virtual display may be a new one.
    if ($script:vdMode -and $script:vdName) { Update-VirtualScreen | Out-Null }
}

$rows = @()
$jsonl = Join-Path $ResultsDir 'display.jsonl'
if (Test-Path $jsonl) { Remove-Item $jsonl }
function Add-Row($Row) {
    $o = [pscustomobject]$Row
    $mark = if ($null -eq $o.ok) { '--' } elseif ($o.ok) { 'OK' } else { 'KO' }
    Write-Host ("  [{0}] {1}: {2}" -f $mark, $o.id, $o.observed)
    if ($o.reason) { Write-Host "       $($o.reason)" }
    ($o | ConvertTo-Json -Compress -Depth 6) | Add-Content -Path $jsonl -Encoding UTF8
    $script:rows += $o
}

# -- The guest (-Share) -----------------------------------------------------
# What the guest's worker encodes. Its log is the worker file that appeared
# when the guest joined: found once, then kept out of the owner's lines.
function Get-GuestState($Mark) {
    if (-not $script:GuestLog) {
        $new = @(Get-WorkerLogs | Where-Object { -not $Mark.ContainsKey($_.FullName) } |
            Where-Object { Select-String -Path $_.FullName -Pattern '\[native\] session:' -Quiet })
        if ($new.Count -ne 1) { return Get-HostState @() }
        $script:GuestLog = $new[0].FullName
    }
    return Get-HostState (Get-HostLines $Mark $script:GuestLog)
}

function Invoke-ShareApi([string] $Path, [string] $Body = '{}') {
    $key = (Invoke-RestMethod -Uri "$ApiUrl/api/admin/token" -TimeoutSec 10).token
    Invoke-RestMethod -Uri "$ApiUrl$Path" -Method Post -ContentType 'application/json' `
        -Headers @{ 'X-MW-Admin-Key' = $key } -Body $Body -TimeoutSec 15
}

# Open a share on the running stream and have the guest kiosk join it the way a
# person does: the link, the PIN, the Join button.
function Join-Guest {
    $act = Invoke-ShareApi '/api/share/slots/2/activate' '{"ttl_secs":3600}'
    $token = Get-Prop $act 'token' ''
    # The link names the token in its path on the LAN (/p/<token>), in its
    # fragment through the introduction server (#t=<token>). The guest kiosk
    # opens it on this machine, by path, either way.
    $link = Get-Prop $act 'url' ''
    if (-not $token -and $link -match '/p/([^/?#]+)') { $token = $Matches[1] }
    if (-not $token -and $link -match '#t=([^&]+)') { $token = $Matches[1] }
    if (-not $token) { throw "the share opened without a link: $($act | ConvertTo-Json -Compress)" }
    $guestUrl = $AppUrl.TrimEnd('/') + '/p/' + $token
    $guestArgs = @('-Url', $guestUrl, '-X', $script:guestRect[0], '-Y', $script:guestRect[1],
                   '-W', $script:guestRect[2], '-H', $script:guestRect[3], '-DebugPort', $GuestDebugPort,
                   '-ChromeProfile', '.chrome-guest', '-Windowed')
    if ($ClientAdapterLuid) { $guestArgs += @('-AdapterLuid', $ClientAdapterLuid) }
    & powershell -NoProfile -File "$PSScriptRoot\kiosk.ps1" @guestArgs | Out-Host
    $ready = Wait-For { (CdpAt $GuestDebugPort eval "!!document.querySelector('.player-pin-input')") -match 'true' } 40
    if ($ready -lt 0) { throw "the guest page never asked for the PIN at $guestUrl" }
    CdpAt $GuestDebugPort eval ("(() => { const i = document.querySelector('.player-pin-input'); i.value = '" + $act.pin +
        "'; i.form.requestSubmit(); return 'sent'; })()") | Out-Null
    $ready = Wait-For { (CdpAt $GuestDebugPort eval "!!document.querySelector('.player-join-btn')") -match 'true' } 20
    if ($ready -lt 0) { throw 'the guest page never offered to join after the PIN' }
    CdpAt $GuestDebugPort clicksel '.player-join-btn' | Out-Null
    # The owner's page learns of the share by polling (every five seconds):
    # until then nothing on it knows the stream is shared.
    Start-Sleep -Seconds 7
}

# A change and its verdict. $Act changes a screen; $Expect is evaluated against
# the host state, the view and the page lines, polled until it holds or the
# timeout. The counts are taken over a settling window AFTER it holds, because
# a decoder created twice more a second later is exactly the defect to catch.
function Invoke-Change {
    param([string] $Id, [string] $What, [string] $Expected, [scriptblock] $Act,
          [scriptblock] $Holds, [int] $MaxDecoders = 1, [int] $Relaunches = -1, [switch] $Launch,
          [scriptblock] $GuestHolds = $null,
          # Asked right after $Act: a reason when the change could not be made
          # as described, and the row then says so instead of judging it.
          [scriptblock] $Void = $null)
    Write-Host ""
    Write-Host "=== $Id - $What ==="
    # Each change is measured on a live stream: one that died on the previous
    # change is reported there, and must not turn every later change into the
    # same failure.
    if (-not $Launch -and (Get-View).w -eq 0) {
        Write-Host '  no stream left from the previous change - launching again'
        Reset-Page
        Start-Stream
    }
    $hostMark = Get-LogMark
    $since = Get-PageNow
    & $Act
    $voidWhy = if ($Void) { & $Void } else { '' }
    if ($voidWhy) {
        Add-Row ([ordered]@{ id = $Id; what = $What; expected = $Expected; observed = 'not judged'; ok = $null
                             reason = $voidWhy; adaptMs = -1; decoders = 0; relaunches = 0; evidence = @() })
        return
    }
    $script:hostState = $null
    $ms = Wait-For {
        $script:hostState = Get-HostState (Get-HostLines $hostMark)
        $script:view = Get-View
        $script:page = Get-PageLines $since
        if ($GuestHolds) {
            $script:guestMark = $hostMark
            $script:guestState = Get-GuestState $hostMark
            $script:guestView = Get-View $GuestDebugPort
            (& $Holds) -and (& $GuestHolds)
        } else {
            & $Holds
        }
    }
    if ($ms -ge 0) { Start-Sleep -Seconds 5 }
    $hs = Get-HostState (Get-HostLines $hostMark)
    $v = Get-View
    $p = Get-PageLines $since
    $guestNote = ''
    $guestLines = @()
    if ($GuestHolds) {
        $gs = Get-GuestState $hostMark
        $gv = Get-View $GuestDebugPort
        $guestNote = "; guest: host encodes {0}x{1} {2}, page draws {3}x{4}" -f $gs.w, $gs.h,
            $(if ($gs.hdr) { 'HDR' } else { 'SDR' }), $gv.w, $gv.h
        if ($script:GuestLog) { $guestLines = @(Get-HostLines $hostMark $script:GuestLog | ForEach-Object { "guest $_" }) }
    }
    $decoders = @($p | Where-Object { $_ -match 'starting a new decoder' }).Count
    $switches = @($p | Where-Object { $_ -match 'Seamless transition: first frame on standby' }).Count
    $reasons = @()
    if ($v.w -eq 0) { $reasons += 'the stream ended (the host log says why)' }
    if ($GuestHolds -and $gv.w -eq 0) { $reasons += "the guest's stream ended" }
    if ($ms -lt 0) { $reasons += "not within $TimeoutSec s" }
    if ($decoders -gt $MaxDecoders) { $reasons += "$decoders new decoders where $MaxDecoders was enough" }
    if ($Relaunches -ge 0 -and $switches -ne $Relaunches) { $reasons += "$switches seamless relaunch(es), expected $Relaunches" }
    Add-Row ([ordered]@{
        id = $Id; what = $What; expected = $Expected
        # The host states its display only when it CHANGES; a launch has none.
        observed = ("host encodes {0}x{1} {2}{3}; page draws {4}x{5} ({6}), its screen {7}" -f
            $hs.w, $hs.h, $(if ($hs.hdr) { 'HDR' } else { 'SDR' }),
            $(if ($hs.displayW -gt 0) { ", display {0}x{1} {2}" -f $hs.displayW, $hs.displayH, $(if ($hs.displayHdr) { 'HDR' } else { 'SDR' }) } else { '' }),
            $v.w, $v.h, $v.sink, $(if ($v.screenHdr) { 'HDR' } else { 'SDR' })) + $guestNote
        ok = ($reasons.Count -eq 0); reason = ($reasons -join '; ')
        adaptMs = $ms; decoders = $decoders; relaunches = $switches
        evidence = @(@(Get-HostLines $hostMark) + $guestLines + @($p | Where-Object {
            $_ -match 'Host display|Native HDR|Native host: HDR|Seamless transition|new decoder|First decoded frame|renderer=|\[bench\]' }) |
            Select-Object -Last 18)
    })
}

function Skip-Change([string] $Id, [string] $What, [string] $Why) {
    Write-Host ""
    Write-Host "=== $Id - $What ==="
    Add-Row ([ordered]@{ id = $Id; what = $What; expected = ''; observed = 'not run'; ok = $null
                         reason = $Why; adaptMs = -1; decoders = 0; relaunches = 0; evidence = @() })
}

# -- Run ------------------------------------------------------------------------
# What the screens were, to put them back. The virtual display's is taken once
# a stream has made it.
function New-Orig {
    [pscustomobject]@{ capW = $cap.W; capH = $cap.H; capHz = $cap.Hz; capHdr = $cap.HdrOn
                       cliW = $client.W; cliH = $client.H; cliHz = $client.Hz; cliHdr = $client.HdrOn }
}
$orig = if ($cap) { New-Orig } else { $null }
if ($script:vdMode) { $alt = $null }
$kiosksUp = $false
$script:shareUp = $false
$script:guestState = $null
$script:guestView = $null
try {
    if ($cap -and $cap.HdrOn) { Set-ScreenHdr $Device $false }

    # The kiosks: moving content on the captured screen, the app on the client's.
    # The virtual display gets its content once a stream has brought it up.
    $content = 'file:///' + ((Join-Path $PSScriptRoot 'content\scroll.html') -replace '\\', '/')
    if (-not $script:vdMode) {
        & powershell -NoProfile -File "$PSScriptRoot\kiosk.ps1" -Url $content -X $cap.X -Y $cap.Y -W $cap.W -H $cap.H | Out-Host
    }
    # Shared, the client's screen is split: the owner on the left half, the
    # guest (Join-Guest) on the right.
    $ownerW = if ($Share) { [int][Math]::Floor($client.W / 2) } else { $client.W }
    $script:guestRect = @(($client.X + $ownerW), $client.Y, ($client.W - $ownerW), $client.H)
    # An empty -AdapterLuid is a missing argument to a child powershell: only when set.
    $clientArgs = @('-Url', $AppUrl, '-X', $client.X, '-Y', $client.Y, '-W', $ownerW, '-H', $client.H,
                    '-DebugPort', $DebugPort)
    if ($ClientAdapterLuid) { $clientArgs += @('-AdapterLuid', $ClientAdapterLuid) }
    if ($Share) { $clientArgs += '-Windowed' }
    & powershell -NoProfile -File "$PSScriptRoot\kiosk.ps1" @clientArgs | Out-Host
    $kiosksUp = $true

    # Aspect Auto is what hands the shape to the host. HDR is asked OFF on
    # purpose: a native host ignores the box, and a stream that still comes
    # out HDR below is the proof.
    #
    # The virtual display is made at the size the launch names, and its other
    # modes are all 16:9: a 4:3 custom size makes it 4:3, and gives the shape
    # change a real other shape to go to (a game's 4:3 the other way round).
    $settingsPath = Join-Path $ResultsDir 'settings-display-follow.json'
    $size = if ($script:vdMode) { '"stream_resolution":"custom","stream_custom_width":1440,"stream_custom_height":1080' }
            else { '"stream_height":1080,"stream_aspect":"auto"' }
    ('{"video_codec":"hevc",' + $size + ',"stream_fps":60,"hdr_enabled":false,"mute_host_audio":true}') |
        Set-Content -Path $settingsPath -Encoding ASCII
    $ready = Wait-For { (Cdp eval 'document.body ? document.body.innerText : 0') -match [regex]::Escape($Tile) } 80
    if ($ready -lt 0) { throw "the app at $AppUrl never showed a tile named '$Tile' - is the host paired in this instance?" }
    # The service worker would otherwise serve the kiosk profile the JS of the
    # last build it saw, and a fix to the app would be measured on the old app.
    Cdp eval "Promise.all([caches.keys().then(k => Promise.all(k.map(x => caches.delete(x)))), navigator.serviceWorker.getRegistrations().then(r => Promise.all(r.map(x => x.unregister())))]).then(() => 'purged')" | Out-Null
    Cdp settingsfile $settingsPath | Out-Null
    $ready = Wait-For { (Cdp eval 'document.body ? document.body.innerText : 0') -match [regex]::Escape($Tile) } 60
    if ($ready -lt 0) { throw "after the settings reload the tile '$Tile' never came back" }
    Cdp evalfile $hookJs | Out-Null

    # Whether this client can show HDR at all is the page's own verdict, which
    # it logs at launch: the screen, WebGPU and a 10-bit decoder.
    $launchSince = Get-PageNow
    $launchWhat = if ($cap) { "launch on a $($cap.W)x$($cap.H) SDR display" } else { 'launch on the virtual display, made SDR' }
    Invoke-Change 'launch-sdr' $launchWhat `
        "the display's shape, SDR" `
        { Start-Stream } `
        { (Update-VirtualScreen) -and $script:hostState.w -gt 0 -and $null -ne $script:hostState.hdr -and
          (Test-Ratio $script:hostState.w $script:hostState.h $cap.W $cap.H) -and
          -not $script:hostState.hdr -and $script:view.w -eq $script:hostState.w -and $script:view.h -eq $script:hostState.h } `
        -MaxDecoders 0 -Relaunches 0 -Launch
    if ($script:vdMode) {
        if (-not (Update-VirtualScreen)) { throw "the stream on '$Tile' brought no new screen up" }
        $alt = Get-AltMode
        if (-not $alt) { throw "$Device offers no mode of another shape than $($cap.W)x$($cap.H)" }
        $orig = New-Orig
        Write-Host "captured     : $Device ($($cap.Name)) $($cap.W)x$($cap.H)@$($cap.Hz) HDR=$($cap.HdrOn)"
        Write-Host "other shape  : $($alt[0])x$($alt[1])"
        & powershell -NoProfile -File "$PSScriptRoot\kiosk.ps1" -Url $content -X $cap.X -Y $cap.Y -W $cap.W -H $cap.H | Out-Host
    }
    $askLine = @(Get-PageLines $launchSince | Where-Object { $_ -match 'Native host: HDR' }) | Select-Object -Last 1
    $clientHdr = [bool]($askLine -match 'display=true webgpu=true decode=true')
    Write-Host "client HDR   : $clientHdr ($askLine)"

    # Shared, every change below is also a verdict on the guest: still
    # streaming, and (for a shape) at the display's new shape.
    $guestAlive = if ($Share) { { $script:guestView.w -gt 0 } } else { $null }
    $guestShapeOther = $null
    $guestShapeBack = $null
    if ($Share) {
        $script:shareUp = $true
        Invoke-Change 'share-join' 'a guest joins the stream through a share link' `
            "the guest streams the display's shape; the owner's stream untouched" `
            { Join-Guest } `
            { $script:view.w -gt 0 } `
            -MaxDecoders 0 -Relaunches 0 `
            -GuestHolds { $script:guestState.w -gt 0 -and
                          (Test-Ratio $script:guestState.w $script:guestState.h $cap.W $cap.H) -and
                          $script:guestView.w -eq $script:guestState.w -and $script:guestView.h -eq $script:guestState.h }
        $guestShapeOther = { (Test-Ratio $script:guestState.w $script:guestState.h $alt[0] $alt[1]) -and
                             $script:guestView.w -eq $script:guestState.w -and $script:guestView.h -eq $script:guestState.h }
        $guestShapeBack = { (Test-Ratio $script:guestState.w $script:guestState.h $orig.capW $orig.capH) -and
                            $script:guestView.w -eq $script:guestState.w -and $script:guestView.h -eq $script:guestState.h }
    }

    Invoke-Change 'shape-other' "the display switches to $($alt[0])x$($alt[1]) mid-stream" `
        "the stream rebuilt at that shape, one new decoder, no relaunch" `
        { Set-ScreenMode $Device $alt[0] $alt[1] } `
        { (Test-Ratio $script:hostState.w $script:hostState.h $alt[0] $alt[1]) -and
          $script:view.w -eq $script:hostState.w -and $script:view.h -eq $script:hostState.h } `
        -MaxDecoders 1 -Relaunches 0 -GuestHolds $guestShapeOther

    Invoke-Change 'shape-back' "the display goes back to $($orig.capW)x$($orig.capH)" `
        "the stream back at the first shape, one new decoder, no relaunch" `
        { Set-ScreenMode $Device $orig.capW $orig.capH $orig.capHz } `
        { (Test-Ratio $script:hostState.w $script:hostState.h $orig.capW $orig.capH) -and
          $script:view.w -eq $script:hostState.w -and $script:view.h -eq $script:hostState.h } `
        -MaxDecoders 1 -Relaunches 0 -GuestHolds $guestShapeBack

    $flipWhy = if ($NoClientFlip) { '-NoClientFlip' }
               elseif (-not $client.HdrSupported) { "the client's screen $($client.Gdi) cannot do HDR" }
               elseif (-not $client.HdrOn) { "the client's screen is SDR, so the client side was covered by hdr-host-on already" }
               elseif (-not $clientHdr) { 'this client cannot show HDR even on an HDR screen (WebGPU or the 10-bit decoder)' }
               else { '' }

    if ($Share -and $cap.HdrSupported) {
        # The host's change goes through the share; the client's is held back
        # until the share is over.
        $want = $clientHdr
        Invoke-Change 'hdr-host-on-shared' 'the host display enters HDR while the stream is shared' `
            $(if ($want) { "the owner's stream relaunched in HDR, the guest still streaming" } else { 'still SDR (this client cannot show HDR), the guest still streaming' }) `
            { Set-ScreenHdr $Device $true } `
            { $script:hostState.displayHdr -eq $true -and $script:hostState.hdr -eq $want } `
            -MaxDecoders 1 -Relaunches $(if ($want) { 1 } else { 0 }) -GuestHolds $guestAlive
        if ($flipWhy) {
            Skip-Change 'hdr-client-off-shared' "the client's screen leaves HDR while shared" $flipWhy
            Invoke-Change 'hdr-host-off-shared' 'the host display leaves HDR while the stream is shared' `
                $(if ($want) { "the owner's stream relaunched in SDR, the guest still streaming" } else { 'still SDR, the guest still streaming' }) `
                { Set-ScreenHdr $Device $false } `
                { $script:hostState.displayHdr -eq $false -and $script:hostState.hdr -eq $false } `
                -MaxDecoders 1 -Relaunches $(if ($want) { 1 } else { 0 }) -GuestHolds $guestAlive
        } else {
            Invoke-Change 'hdr-client-off-shared' "the client's screen leaves HDR while the stream is shared" `
                'held back: no relaunch, the stream still HDR, the guest still streaming' `
                { Set-ScreenHdr $client.Gdi $false } `
                { -not $script:view.screenHdr -and $script:hostState.hdr -ne $false -and
                  @($script:page | Where-Object { $_ -match 'held back while a share is active' }).Count -gt 0 } `
                -MaxDecoders 0 -Relaunches 0 -GuestHolds $guestAlive
            Invoke-Change 'share-over' 'the share ends with that change still held back' `
                'the stream relaunched in SDR once the owner page sees the share gone' `
                { Invoke-ShareApi '/api/share/slots/2/deactivate' | Out-Null; $script:shareUp = $false } `
                { $script:hostState.hdr -eq $false -and
                  @($script:page | Where-Object { $_ -match 'Native HDR \(share over\)' }).Count -gt 0 } `
                -MaxDecoders 1 -Relaunches 1
            Invoke-Change 'hdr-client-on' "the client's screen enters HDR again, nothing shared" `
                'the stream relaunched in HDR' `
                { Set-ScreenHdr $client.Gdi $true
                  Set-ScreenMode $client.Gdi $orig.cliW $orig.cliH $orig.cliHz } `
                { $script:view.screenHdr -and $script:hostState.hdr -eq $true } `
                -MaxDecoders 1 -Relaunches 1
        }
    } elseif ($Share) {
        Skip-Change 'hdr-host-on-shared' 'the host display enters HDR while shared' "$Device cannot do HDR"
    } elseif (-not $cap.HdrSupported) {
        Skip-Change 'hdr-host-on' 'the host display enters HDR' "$Device cannot do HDR"
    } else {
        $want = $clientHdr
        Invoke-Change 'hdr-host-on' 'the host display enters HDR mid-stream' `
            $(if ($want) { 'the stream relaunched in HDR' } else { 'still SDR (this client cannot show HDR): converted on the host, no relaunch' }) `
            { Set-ScreenHdr $Device $true } `
            { $script:hostState.displayHdr -eq $true -and $script:hostState.hdr -eq $want } `
            -MaxDecoders 1 -Relaunches $(if ($want) { 1 } else { 0 })

        if ($flipWhy) {
            Skip-Change 'hdr-client-off' "the client's screen leaves HDR" $flipWhy
        } else {
            Invoke-Change 'hdr-client-off' "the client's screen leaves HDR mid-stream" `
                'the stream relaunched in SDR, converted on the host' `
                { Set-ScreenHdr $client.Gdi $false } `
                { -not $script:view.screenHdr -and $script:hostState.hdr -eq $false } `
                -MaxDecoders 1 -Relaunches 1
            Invoke-Change 'hdr-client-on' "the client's screen enters HDR again" `
                'the stream relaunched in HDR' `
                { Set-ScreenHdr $client.Gdi $true
                  Set-ScreenMode $client.Gdi $orig.cliW $orig.cliH $orig.cliHz } `
                { $script:view.screenHdr -and $script:hostState.hdr -eq $true } `
                -MaxDecoders 1 -Relaunches 1
        }

        Invoke-Change 'hdr-host-off' 'the host display leaves HDR mid-stream' `
            $(if ($want) { 'the stream relaunched in SDR' } else { 'still SDR, no relaunch' }) `
            { Set-ScreenHdr $Device $false } `
            { $script:hostState.displayHdr -eq $false -and $script:hostState.hdr -eq $false } `
            -MaxDecoders 1 -Relaunches $(if ($want) { 1 } else { 0 })

        if ($script:vdMode) {
            # The virtual display exists only while a stream holds it, and is
            # made SDR whenever it comes up: it goes to HDR under the running
            # stream, and the next launch lands inside the 4 s it outlives it.
            Set-ScreenHdr $Device $true
            Start-Sleep -Seconds 4
            $relaunch = {
                $r = Cdp relaunch $Tile $hookJs
                Write-Host "  relaunched: $($r.Trim())"
                $ms = Wait-For { (Get-View).w -gt 0 } 40
                if ($ms -lt 0) { throw "the stream on '$Tile' never showed a picture after the relaunch" }
                Cdp fullscreen | Out-Null
                Start-Sleep -Seconds 3
                Update-VirtualScreen | Out-Null
            }
            $void = { $s = Get-Screen $Device
                      if (-not $s -or -not $s.HdrOn) {
                        'the virtual display went off between the two streams (the 4 s it outlives the last one ' +
                        'were missed) and came back SDR, as every activation makes it' } }
        } else {
            Reset-Page
            Set-ScreenHdr $Device $true
            Start-Sleep -Seconds 4
            $relaunch = { Start-Stream }
            $void = $null
        }
        Invoke-Change 'launch-hdr' 'launch on a display already in HDR, the box unticked' `
            $(if ($want) { 'HDR from the first frame' } else { 'SDR: this client cannot show HDR' }) `
            $relaunch `
            { $script:hostState.sessions -gt 0 -and $script:hostState.hdr -eq $want } `
            -MaxDecoders 0 -Relaunches 0 -Launch -Void $void
    }
    Reset-Page
}
finally {
    Write-Host ""
    if ($Share -and $script:shareUp) {
        try { Invoke-ShareApi '/api/share/slots/2/deactivate' | Out-Null; Write-Host 'share closed' }
        catch { Write-Warning "could not close the share: $_" }
    }
    # A virtual display that never came up left nothing to put back. One that
    # did is put back too: it outlives the last stream by 4 s.
    if ($orig) { try {
        $now = if ($Device) { Get-Screen $Device } else { $null }
        if ($now -and ($now.W -ne $orig.capW -or $now.H -ne $orig.capH)) { Set-ScreenMode $Device $orig.capW $orig.capH $orig.capHz }
        if ($now -and $now.HdrOn -ne $orig.capHdr) { Set-ScreenHdr $Device $orig.capHdr }
        $now = Get-Screen $client.Gdi
        if ($now -and $now.HdrOn -ne $orig.cliHdr) { Set-ScreenHdr $client.Gdi $orig.cliHdr }
        $now = Get-Screen $client.Gdi
        # HDR can drop a monitor's refresh rate and leave it there.
        if ($now -and ($now.W -ne $orig.cliW -or $now.H -ne $orig.cliH -or $now.Hz -ne $orig.cliHz)) {
            Set-ScreenMode $client.Gdi $orig.cliW $orig.cliH $orig.cliHz
        }
        Write-Host "screens restored: $Device $($orig.capW)x$($orig.capH) HDR=$($orig.capHdr), $($client.Gdi) HDR=$($orig.cliHdr) @$($orig.cliHz) Hz"
    } catch {
        Write-Warning "could not put the screens back: $_"
    } }
    if ($kiosksUp -and -not $KeepKiosks) {
        & powershell -NoProfile -File "$PSScriptRoot\kiosk-close.ps1" | Out-Host
    }
}

$ko = @($rows | Where-Object { $_.ok -eq $false }).Count
Write-Host ""
Write-Host "display follow: $($rows.Count) changes, $ko KO - written to $jsonl"
