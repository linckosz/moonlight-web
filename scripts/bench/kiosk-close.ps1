# ============================================================================
# Take the bench windows off the screen, unconditionally.
#
#   .\kiosk-close.ps1
#
# This is the escape hatch, and it exists because the kiosk deliberately has no
# way out of its own: it is a borderless --kiosk Chrome pinned TOPMOST, so it
# has no close button, and Alt+F4 goes to whatever window holds the focus -
# which is behind the overlay, where nobody can see what they just closed.
# A campaign that stops early (an error, a Ctrl+C, an operator saying stop)
# used to leave that window on the machine with no way to get rid of it.
#
# Nothing here is clever on purpose: it must work when Chrome is wedged, when
# the hotkey watchdog is dead, and when the person running it has no idea which
# of the two kiosks is which.
# ============================================================================
param(
    # By default both kiosks and the click target. -Content / -Client narrow it.
    [switch] $Content,
    [switch] $Client
)

$tags = @()
if ($Content) { $tags += '.chrome-bench' }
if ($Client) { $tags += '.chrome-client' }
if (-not $tags) { $tags = @('.chrome-bench', '.chrome-client') }

$killed = 0
Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" | ForEach-Object {
    $cmd = $_.CommandLine
    foreach ($t in $tags) {
        if ($cmd -like "*$t*") {
            Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
            $killed++
            break
        }
    }
}

# The helpers the kiosks bring with them: the invisible window the pointer is
# parked in, and the hotkey watchdog. Both are headless, so a stale one is
# invisible and would otherwise be found only by name, months later.
#
# The patterns end in .ps1 on purpose. '*kiosk-hotkeys*' alone also matches any
# shell that merely NAMES the thing - a command mentioning the watchdog's log
# file was enough - and this script then killed the session that called it,
# which looks exactly like a crash. Requiring the script's own file name makes
# an accidental match need someone to type the path.
# Excluding $PID is still needed: this shell's own command line names them too.
$helpers = 0
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
    Where-Object {
        $_.ProcessId -ne $PID -and
        ($_.CommandLine -like '*click-target.ps1*' -or $_.CommandLine -like '*kiosk-hotkeys.ps1*')
    } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; $helpers++ }

"closed $killed kiosk process(es) and $helpers helper(s)"
