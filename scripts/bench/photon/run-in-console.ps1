# Run click-photon.ps1 in the console session of a Windows client reached over
# ssh (an ssh session lands in session 0, which has no desktop: no pixel to
# read, no window to click). A one-shot Interactive scheduled task, waited for,
# then its output printed and the task removed.
#   powershell -File run-in-console.ps1 -Tag steam-hevc-1 [-Process streaming_client] [-Title ...] [-CdpPort 9222] [-Clicks 60]
# -CdpPort: the bench Chrome by the port it is driven on, when several windows match.
param(
    [Parameter(Mandatory)] [string] $Tag,
    [string] $Process = 'streaming_client',
    [string] $Title = '',
    [int] $CdpPort = 0,
    [int] $Clicks = 60,
    [int] $ReadDx = 0,
    [int] $ReadDy = 0,
    [string] $Dir = $PSScriptRoot
)
$script = Join-Path $Dir 'click-photon.ps1'
$out = Join-Path $Dir "$Tag.json"
$log = Join-Path $Dir "$Tag.txt"
$task = 'MwClickPhoton'
$args = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command `"& '$script' -Process '$Process' " +
        "-Title '$Title' -CdpPort $CdpPort -Clicks $Clicks -ReadDx $ReadDx -ReadDy $ReadDy -Out '$out' *> '$log'`""
Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
$action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $args
$user = (Get-CimInstance Win32_ComputerSystem).UserName
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive
Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Force | Out-Null
Remove-Item $log, $out -ErrorAction SilentlyContinue
Start-ScheduledTask -TaskName $task
$deadline = (Get-Date).AddSeconds(30 + $Clicks * 2)
do { Start-Sleep -Seconds 2 } while ((Get-ScheduledTask -TaskName $task).State -eq 'Running' -and (Get-Date) -lt $deadline)
Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
if (Test-Path $log) { Get-Content $log } else { "no output ($log)" }
