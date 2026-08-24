<#
.SYNOPSIS
    Buzz an XInput pad from the host, to exercise MoonlightWeb's rumble path.

.DESCRIPTION
    Rumble travels the opposite way to every other input: the game asks the pad
    to vibrate, the pad here is the virtual one Sunshine creates, and the request
    has to make it back down the stream to the browser, which replays it on the
    real controller through the Gamepad API's vibrationActuator.

    That return path cannot be tested with joy.cpl. joy.cpl talks to a PHYSICAL
    pad attached to the machine it runs on, and its force-feedback page only
    exists for DirectInput FFB devices (wheels, old sticks) -- an XInput pad,
    virtual or not, shows no such page. So it proves the pad works with Windows,
    and nothing at all about the stream.

    This script instead calls XInputSetState against the virtual pad, which is
    exactly what a game does. ViGEm hands the request to Sunshine, Sunshine sends
    it over the control stream, and MoonlightWeb should make the controller in
    your hands buzz.

.EXAMPLE
    # 1. Before streaming: note which slots already exist (your physical pads).
    .\test-rumble.ps1 -List

.EXAMPLE
    # 2. Start a stream, press a button so the browser exposes the pad, then:
    .\test-rumble.ps1
    # A slot that was not in step 1 is the virtual pad. When that one is buzzed,
    # the controller in your hands should vibrate.

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
    [switch] $List
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
