# Real-time socket monitor for mw-server (TCP + UDP endpoints).
# Run in an ADMIN PowerShell:  powershell -ExecutionPolicy Bypass -File D:\Code\moonlight-web-deepseek\mw-netmon.ps1
$proc = Get-Process mw-server -ErrorAction SilentlyContinue
if (-not $proc) { Write-Host "mw-server is not running."; exit }
$mwpid = $proc.Id
Write-Host "Monitoring mw-server PID $mwpid (Ctrl+C to stop)"
while ($true) {
    Write-Host "==== $(Get-Date -Format HH:mm:ss) ===="
    Get-NetTCPConnection -OwningProcess $mwpid -ErrorAction SilentlyContinue |
        Select-Object @{n='P';e={'TCP'}}, LocalPort, RemoteAddress, RemotePort, State |
        Sort-Object RemoteAddress | Format-Table -AutoSize
    Get-NetUDPEndpoint -OwningProcess $mwpid -ErrorAction SilentlyContinue |
        Select-Object @{n='P';e={'UDP'}}, LocalAddress, LocalPort | Format-Table -AutoSize
    Start-Sleep -Seconds 2
}
