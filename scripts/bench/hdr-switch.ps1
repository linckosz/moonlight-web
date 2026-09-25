# ============================================================================
# Windows HDR on the physical screens a campaign measures, for the ONE pass
# that needs it (hdr-on), and back the way they were, whatever happens.
#
#   . "$PSScriptRoot\hdr-switch.ps1"
#   $was = Enter-PassHdr '\\.\DISPLAY5'      # HDR on, returns what it found
#   try { ...the pass... } finally { Exit-PassHdr '\\.\DISPLAY5' $was }
#
# Dot-sourced by run-matrix.ps1 (the encoder half: the captured screen) and
# run-browser.ps1 (the browser half: the captured screen, and the client's,
# since an HDR stream needs a screen that can show it at both ends).
#
# Physical, because click-to-photon needs a physical screen (the flag is never
# painted on a virtual one); by GDI name, because a desk of three identical
# monitors cannot be told apart by model name. Bruno allowed exactly this on
# 25/09/2026: HDR, and always restored. The MODE is restored too, never
# changed on purpose: some monitors leave HDR at a lower refresh rate than
# they entered it (the M27Q goes from 144 Hz to 60 Hz and stays there). A
# finally runs on Ctrl+C too; a process killed outright is the one case it
# cannot cover, and `scripts\Set-DisplayHdr.ps1 -List` says afterwards what
# was left.
# ============================================================================

$script:SetDisplayHdr = Join-Path (Split-Path $PSScriptRoot -Parent) 'Set-DisplayHdr.ps1'

if (-not ([System.Management.Automation.PSTypeName]'PassHdrMode').Type) {
    Add-Type @"
using System; using System.Runtime.InteropServices;
public static class PassHdrMode {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DEVMODE {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
        public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels;
        public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
        public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2;
        public int dmPanningWidth, dmPanningHeight;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool EnumDisplaySettingsW(string dev, int mode, ref DEVMODE dm);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int ChangeDisplaySettingsExW(string dev, ref DEVMODE dm, IntPtr hwnd, int flags, IntPtr lp);
    public static int[] Get(string dev) {
        var dm = new DEVMODE(); dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
        if (!EnumDisplaySettingsW(dev, -1, ref dm)) return null;
        return new int[] { dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency };
    }
    public static int Set(string dev, int w, int h, int hz) {
        var dm = new DEVMODE(); dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
        EnumDisplaySettingsW(dev, -1, ref dm);
        dm.dmPelsWidth = w; dm.dmPelsHeight = h; dm.dmDisplayFrequency = hz;
        dm.dmFields = 0x80000 | 0x100000 | 0x400000;
        return ChangeDisplaySettingsExW(dev, ref dm, IntPtr.Zero, 0, IntPtr.Zero);
    }
}
"@
}

function Get-PassHdr([string] $Device) {
    $line = & powershell -NoProfile -File $script:SetDisplayHdr -List |
        Where-Object { $_ -like "$Device *" } | Select-Object -First 1
    if (-not $line) { throw "no active display named $Device" }
    return [bool]($line -match 'enabled=True')
}

# The adapter driving a screen, as "high,low" in decimal (the form Chrome's
# --use-adapter-luid and the bench's -ClientAdapterLuid take). Empty when the
# screen is not active.
function Get-ScreenAdapter([string] $Device) {
    $line = & powershell -NoProfile -File $script:SetDisplayHdr -List |
        Where-Object { $_ -like "$Device *" } | Select-Object -First 1
    # render= first: a virtual display's path names its own adapter, not the
    # GPU a client on it decodes with (the Arc, behind "VDD by MTT").
    if ($line -match 'render=(-?\d+),(\d+)') { return "$($Matches[1]),$($Matches[2])" }
    if ($line -match 'adapter=(-?\d+),(\d+)') { return "$($Matches[1]),$($Matches[2])" }
    return ''
}

function Set-PassHdr([string] $Device, [bool] $On) {
    $switch = if ($On) { '-Enable' } else { '-Disable' }
    & powershell -NoProfile -File $script:SetDisplayHdr -Device $Device $switch | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "could not turn HDR $(if ($On) { 'on' } else { 'off' }) on $Device" }
}

# HDR on for the pass; returns what the screen was, for Exit-PassHdr.
function Enter-PassHdr([string] $Device) {
    $was = [pscustomobject]@{ Hdr = (Get-PassHdr $Device); Mode = [PassHdrMode]::Get($Device) }
    if (-not $was.Hdr) {
        Set-PassHdr $Device $true
        # The compositor and the capture settle into the new format.
        Start-Sleep -Seconds 3
        # A screen that left the desktop instead of entering HDR cannot be put
        # back from here (25/09/2026: the Arc's M27Q, which only a monitor
        # power cycle or a reboot brought back). Stop the campaign and say so.
        $still = & powershell -NoProfile -File $script:SetDisplayHdr -List | Where-Object { $_ -like "$Device *" }
        if (-not $still) {
            throw ("$Device LEFT THE DESKTOP when HDR was turned on - it is not active any more. " +
                   "Power-cycle that monitor (or reboot) and never switch HDR on it again at the bench.")
        }
    }
    return $was
}

function Exit-PassHdr([string] $Device, $Was) {
    if ($null -eq $Was) { return }
    try {
        if ((Get-PassHdr $Device) -ne $Was.Hdr) { Set-PassHdr $Device $Was.Hdr }
        $now = [PassHdrMode]::Get($Device)
        if ($Was.Mode -and $now -and (($now -join 'x') -ne ($Was.Mode -join 'x'))) {
            $rc = [PassHdrMode]::Set($Device, $Was.Mode[0], $Was.Mode[1], $Was.Mode[2])
            Write-Host "mode of $Device put back to $($Was.Mode -join 'x') (rc=$rc)"
        }
    } catch {
        Write-Warning "$Device could not be put back (HDR $($Was.Hdr), mode $($Was.Mode -join 'x')): $_ - check scripts\Set-DisplayHdr.ps1 -List"
    }
}
