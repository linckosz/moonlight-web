<#
.SYNOPSIS
    Buzz the host's virtual pad, to exercise MoonlightWeb's rumble path.

.DESCRIPTION
    Rumble travels the opposite way to every other input: the game asks the pad
    to vibrate, the pad here is the virtual one the host creates, and the request
    has to make it back down the stream to the browser, which replays it on the
    real controller through the Gamepad API's vibrationActuator.

    That return path cannot be tested with joy.cpl. joy.cpl talks to a PHYSICAL
    pad attached to the machine it runs on, and its force-feedback page only
    exists for DirectInput FFB devices (wheels, old sticks) -- an XInput pad,
    virtual or not, shows no such page. So it proves the pad works with Windows,
    and nothing at all about the stream.

    TWO PROFILES, TWO WAYS IN -- and this is the trap. The host presents an Xbox
    360 pad for most controllers, and a DualShock 4 when the browser reports a
    PlayStation one. They are not reachable the same way:

      -- Xbox 360: XInputSetState, which is what a game does. Default mode.
      -- DualShock 4: NOT reachable by XInput at all. XInput enumerates XUSB
         devices only, so a virtual DS4 occupies no XInput slot and this script
         in its default mode cannot see it, let alone buzz it. A DS4 is driven
         by writing a HID output report, which is what -Ds4 does.

    Running the default mode against a DS4 session therefore reports "no rumble"
    whatever the stream does. That is the test failing to reach the device, not
    the path being broken -- use -Ds4 there.

.EXAMPLE
    # 1. Before streaming: note which slots already exist (your physical pads).
    .\test-rumble.ps1 -List

.EXAMPLE
    # 2. Start a stream, press a button so the browser exposes the pad, then:
    .\test-rumble.ps1
    # A slot that was not in step 1 is the virtual pad. When that one is buzzed,
    # the controller in your hands should vibrate.

.EXAMPLE
    # Same, for a stream whose pad arrived as a DualShock 4.
    .\test-rumble.ps1 -Ds4

.EXAMPLE
    # Buzz one slot only, harder and longer.
    .\test-rumble.ps1 -Slot 1 -Seconds 3 -Left 65535 -Right 65535

.NOTES
    Motors are always stopped on the way out, including on Ctrl+C -- a pad left
    buzzing keeps buzzing until something tells it to stop.
#>
[CmdletBinding()]
param(
    # XInput slot to buzz (0-3). Omitted: every connected slot, one after another.
    [ValidateRange(-1, 3)]
    [int] $Slot = -1,

    # How long each buzz lasts.
    [ValidateRange(0.1, 30)]
    [double] $Seconds = 1.5,

    # Low-frequency (heavy) motor, 0-65535.
    [ValidateRange(0, 65535)]
    [int] $Left = 40000,

    # High-frequency (light) motor, 0-65535.
    [ValidateRange(0, 65535)]
    [int] $Right = 40000,

    # Only list the connected slots, buzz nothing.
    [switch] $List,

    # Drive the virtual DualShock 4 instead of the XInput slots. Required when
    # the stream's pad arrived as a PlayStation one: a DS4 is invisible to
    # XInput, so the default mode cannot reach it at all.
    [switch] $Ds4
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not ('MwXInput' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class MwXInput
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Vibration { public ushort Left; public ushort Right; }

    [StructLayout(LayoutKind.Sequential)]
    public struct Gamepad
    {
        public ushort Buttons;
        public byte LeftTrigger;
        public byte RightTrigger;
        public short ThumbLX;
        public short ThumbLY;
        public short ThumbRX;
        public short ThumbRY;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct State { public uint PacketNumber; public Gamepad Pad; }

    // xinput1_4.dll ships with Windows 8 and later; nothing to install.
    [DllImport("xinput1_4.dll")]
    public static extern uint XInputGetState(uint index, ref State state);

    [DllImport("xinput1_4.dll")]
    public static extern uint XInputSetState(uint index, ref Vibration vibration);
}
'@
}

if (-not ('MwHid' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

// Enough of the HID and SetupAPI surface to find one virtual DualShock 4 and
// write it an output report. A DS4's motors live in that report -- there is no
// XInput equivalent, which is the whole reason this class exists.
public static class MwHid
{
    [StructLayout(LayoutKind.Sequential)]
    public struct HIDD_ATTRIBUTES
    {
        public int Size; public ushort VendorID; public ushort ProductID; public ushort VersionNumber;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVICE_INTERFACE_DATA
    {
        public int cbSize; public Guid InterfaceClassGuid; public int Flags; public IntPtr Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct HIDP_CAPS
    {
        public ushort Usage; public ushort UsagePage;
        public ushort InputReportByteLength;
        public ushort OutputReportByteLength;
        public ushort FeatureReportByteLength;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 17)] public ushort[] Reserved;
        public ushort NumberLinkCollectionNodes;
        public ushort NumberInputButtonCaps; public ushort NumberInputValueCaps;
        public ushort NumberInputDataIndices;
        public ushort NumberOutputButtonCaps; public ushort NumberOutputValueCaps;
        public ushort NumberOutputDataIndices;
        public ushort NumberFeatureButtonCaps; public ushort NumberFeatureValueCaps;
        public ushort NumberFeatureDataIndices;
    }

    [DllImport("hid.dll")] static extern void HidD_GetHidGuid(out Guid guid);
    [DllImport("hid.dll", SetLastError = true)] static extern bool HidD_GetAttributes(IntPtr h, ref HIDD_ATTRIBUTES a);
    [DllImport("hid.dll")] static extern bool HidD_GetPreparsedData(IntPtr h, out IntPtr pp);
    [DllImport("hid.dll")] static extern bool HidD_FreePreparsedData(IntPtr pp);
    [DllImport("hid.dll")] static extern int HidP_GetCaps(IntPtr pp, ref HIDP_CAPS caps);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr SetupDiGetClassDevs(ref Guid guid, IntPtr enumerator, IntPtr hwnd, int flags);
    [DllImport("setupapi.dll")]
    static extern bool SetupDiEnumDeviceInterfaces(IntPtr set, IntPtr devInfo, ref Guid guid, int index, ref SP_DEVICE_INTERFACE_DATA data);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode)]
    static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr set, ref SP_DEVICE_INTERFACE_DATA data, IntPtr detail, int detailSize, ref int required, IntPtr devInfoData);
    [DllImport("setupapi.dll")]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateFile(string name, uint access, uint share, IntPtr sec, uint disp, uint flags, IntPtr templ);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool WriteFile(IntPtr h, byte[] buf, int n, out int written, IntPtr ov);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

    const uint GENERIC_RW = 0xC0000000;
    const uint SHARE_RW = 3;
    const uint OPEN_EXISTING = 3;
    const int DIGCF_PRESENT_INTERFACE = 0x12;
    static readonly IntPtr INVALID = new IntPtr(-1);

    public static List<string> FindPaths(ushort vid, ushort pid)
    {
        var found = new List<string>();
        Guid hid; HidD_GetHidGuid(out hid);
        IntPtr set = SetupDiGetClassDevs(ref hid, IntPtr.Zero, IntPtr.Zero, DIGCF_PRESENT_INTERFACE);
        if (set == INVALID) return found;
        try
        {
            var iface = new SP_DEVICE_INTERFACE_DATA();
            iface.cbSize = Marshal.SizeOf(typeof(SP_DEVICE_INTERFACE_DATA));
            for (int i = 0; SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref hid, i, ref iface); i++)
            {
                int needed = 0;
                SetupDiGetDeviceInterfaceDetail(set, ref iface, IntPtr.Zero, 0, ref needed, IntPtr.Zero);
                if (needed <= 0) continue;
                IntPtr buf = Marshal.AllocHGlobal(needed);
                try
                {
                    // cbSize of SP_DEVICE_INTERFACE_DETAIL_DATA, which is not the
                    // buffer size: 8 on x64, 6 on x86, because of the trailing
                    // character array's alignment.
                    Marshal.WriteInt32(buf, IntPtr.Size == 8 ? 8 : 6);
                    if (!SetupDiGetDeviceInterfaceDetail(set, ref iface, buf, needed, ref needed, IntPtr.Zero)) continue;
                    string path = Marshal.PtrToStringUni(new IntPtr(buf.ToInt64() + 4));
                    if (String.IsNullOrEmpty(path)) continue;

                    // Query-only open: a pad already held by a game refuses
                    // read/write, and we still want to identify it.
                    IntPtr h = CreateFile(path, 0, SHARE_RW, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
                    if (h == INVALID) continue;
                    try
                    {
                        var attrs = new HIDD_ATTRIBUTES();
                        attrs.Size = Marshal.SizeOf(typeof(HIDD_ATTRIBUTES));
                        if (HidD_GetAttributes(h, ref attrs) && attrs.VendorID == vid && attrs.ProductID == pid)
                            found.Add(path);
                    }
                    finally { CloseHandle(h); }
                }
                finally { Marshal.FreeHGlobal(buf); }
            }
        }
        finally { SetupDiDestroyDeviceInfoList(set); }
        return found;
    }

    // Null on success, else a human-readable reason.
    public static string Rumble(string path, byte weak, byte strong)
    {
        IntPtr h = CreateFile(path, GENERIC_RW, SHARE_RW, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
        if (h == INVALID) return "cannot open the device (error " + Marshal.GetLastWin32Error() + ")";
        try
        {
            int len = 32;
            IntPtr pp;
            if (HidD_GetPreparsedData(h, out pp))
            {
                var caps = new HIDP_CAPS();
                // HIDP_STATUS_SUCCESS
                if (HidP_GetCaps(pp, ref caps) == 0x00110000 && caps.OutputReportByteLength > 0)
                    len = caps.OutputReportByteLength;
                HidD_FreePreparsedData(pp);
            }
            if (len < 6) return "the device declares no usable output report";

            // The DualShock 4's USB output report. Byte 4 is the small (right)
            // motor and byte 5 the large (left) one -- the same two values
            // ViGEmBus hands back to MoonlightWeb as a rumble notification.
            byte[] report = new byte[len];
            report[0] = 0x05;
            report[1] = 0xF7;
            report[2] = 0x04;
            report[3] = 0x00;
            report[4] = weak;
            report[5] = strong;

            // WriteFile, and ONLY WriteFile. It becomes a USB interrupt OUT
            // transfer, which is the one path ViGEmBus reads the motors from
            // (Ds4Pdo.cpp, UsbBulkOrInterruptTransfer). HidD_SetOutputReport
            // becomes a SET_REPORT control transfer instead, which the bus
            // answers "success" to and then discards -- so it looks like it
            // worked and nothing ever vibrates. That is exactly how the first
            // DS4 rumble test on this project concluded "broken" when it was
            // the test that never reached the device. Real pads take WriteFile
            // too (it is what hidapi, SDL and Steam use), so nothing is lost.
            int written;
            if (WriteFile(h, report, report.Length, out written, IntPtr.Zero)) return null;
            return "the device refused the report (WriteFile error " +
                   Marshal.GetLastWin32Error() + ")";
        }
        finally { CloseHandle(h); }
    }
}
'@
}

function Get-ConnectedSlot {
    $found = @()
    for ($i = 0; $i -lt 4; $i++) {
        $state = New-Object 'MwXInput+State'
        if ([MwXInput]::XInputGetState([uint32]$i, [ref]$state) -eq 0) { $found += $i }
    }
    # Leading comma: without it PowerShell unrolls the array and an empty result
    # comes back as $null, which has no .Count under Set-StrictMode.
    return ,$found
}

function Set-Motor {
    param([int] $Index, [int] $LowFreq, [int] $HighFreq)
    $vib = New-Object 'MwXInput+Vibration'
    $vib.Left = [uint16]$LowFreq
    $vib.Right = [uint16]$HighFreq
    [void][MwXInput]::XInputSetState([uint32]$Index, [ref]$vib)
}

if ($Ds4) {
    # ViGEm presents its DualShock 4 as the real thing: Sony's vendor id and the
    # first-generation DS4 product id.
    $paths = [MwHid]::FindPaths([uint16]0x054C, [uint16]0x05C4)

    if ($paths.Count -eq 0) {
        Write-Host "No DualShock 4 found on this machine." -ForegroundColor Yellow
        Write-Host ""
        Write-Host "During a stream the virtual pad only appears once the browser has"
        Write-Host "announced one, and browsers hide a gamepad until it is used: press a"
        Write-Host "button on the controller in the stream window, then run this again."
        Write-Host ""
        Write-Host "If the pad in your hands is NOT a PlayStation one, the host presented"
        Write-Host "an Xbox 360 pad instead -- run this script without -Ds4."
        exit 1
    }

    Write-Host ("DualShock 4 devices found: " + $paths.Count) -ForegroundColor Cyan
    Write-Host "(a physical DS4 attached to this machine would be listed here too)"

    if ($List) { exit 0 }

    # The report carries 8-bit motors; the parameters are 16-bit like XInput's,
    # so the same command line means the same strength in both modes.
    $weak = [byte]([math]::Min(255, [int]($Right / 257)))
    $strong = [byte]([math]::Min(255, [int]($Left / 257)))

    Write-Host ("Buzzing {0}s per device (left={1}, right={2})" -f $Seconds, $Left, $Right)
    Write-Host ""

    $targets = if ($Slot -ge 0) { @($paths[[math]::Min($Slot, $paths.Count - 1)]) } else { $paths }

    try {
        foreach ($path in $targets) {
            Write-Host "  device ... " -NoNewline -ForegroundColor Green
            $err = [MwHid]::Rumble($path, $weak, $strong)
            if ($err) {
                Write-Host $err -ForegroundColor Red
                continue
            }
            Start-Sleep -Milliseconds ([int]($Seconds * 1000))
            [void][MwHid]::Rumble($path, [byte]0, [byte]0)
            Write-Host "done"
            if ($targets.Count -gt 1) { Start-Sleep -Milliseconds 700 }
        }
    } finally {
        foreach ($path in $targets) { [void][MwHid]::Rumble($path, [byte]0, [byte]0) }
    }

    Write-Host ""
    Write-Host "If the controller in your hands buzzed, the whole return path works:"
    Write-Host "  game -> ViGEm (DS4) -> MoonlightWeb -> browser -> your pad."
    Write-Host ""
    Write-Host "If it did not, check the browser console first: a pad whose"
    Write-Host "vibrationActuator is null cannot replay anything, whatever the host sent."
    exit 0
}

$connected = Get-ConnectedSlot

if ($connected.Count -eq 0) {
    Write-Host "No XInput pad on any slot." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "During a stream the virtual pad only appears once the browser has"
    Write-Host "announced one, and browsers hide a gamepad until it is used: press a"
    Write-Host "button on the controller in the stream window, then run this again."
    exit 1
}

Write-Host ("Connected XInput slots: " + ($connected -join ', ')) -ForegroundColor Cyan

if ($List) { exit 0 }

if ($Slot -ge 0) {
    if ($connected -notcontains $Slot) {
        Write-Host "Slot $Slot is not connected." -ForegroundColor Red
        exit 1
    }
    $targets = @($Slot)
} else {
    $targets = @($connected)
}

Write-Host ("Buzzing {0}s per slot (left={1}, right={2})" -f $Seconds, $Left, $Right)
Write-Host ""

try {
    foreach ($index in $targets) {
        Write-Host ("  slot {0} ... " -f $index) -NoNewline -ForegroundColor Green
        Set-Motor -Index $index -LowFreq $Left -HighFreq $Right
        Start-Sleep -Milliseconds ([int]($Seconds * 1000))
        Set-Motor -Index $index -LowFreq 0 -HighFreq 0
        Write-Host "done"
        if ($targets.Count -gt 1) { Start-Sleep -Milliseconds 700 }
    }
} finally {
    # Whatever happened -- Ctrl+C included -- no motor is left running.
    foreach ($index in $targets) { Set-Motor -Index $index -LowFreq 0 -HighFreq 0 }
}

Write-Host ""
Write-Host "If the controller in your hands buzzed, the whole return path works:"
Write-Host "  game -> ViGEm -> Sunshine -> MoonlightWeb -> browser -> your pad."
