"""Stop, uninstall, install and update MoonlightWebDev on one machine.

Chapter 1 of the acceptance run. Every command here is the silent form the
product already uses on itself — the Windows flags are the very string
SelfUpdater.cpp passes (`UpdateSilentArgs`), so what is tested is the path a
user's update actually takes.

A silent uninstall never deletes configuration: the .iss returns early on
`UninstallSilent` before it can ask. That is deliberate and it is what makes
the reinstall-as-update test meaningful — settings have to survive.
"""
import json
import os
import posixpath
import time

import fleet

# The AppId Inno writes for the DEV edition. Looking the UninstallString up by
# AppId beats guessing a path: a machine that installed somewhere else still
# uninstalls, and a machine with nothing installed says so instead of failing.
DEV_APPID = "{5B1E7A8C-8D0E-4F53-8D6F-E1373C70AFE6}_is1"
PROD_APPID = "{6F2C9E4A-7B3D-4E5F-9A1C-2D8E4B6F0A33}_is1"

WIN_SILENT = "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-"
WIN_UNSILENT = "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART"


def _ps_uninstall(appid, app_name):
    """Stop the tasks, kill the process, then run Inno's own uninstaller."""
    return r"""
$appid = '%s'
$name  = '%s'
foreach ($t in @($name, "$name Update", "$name Virtual Display", "$name-dev")) {
    schtasks /End /TN "$t" 2>$null | Out-Null
    schtasks /Delete /TN "$t" /F 2>$null | Out-Null
}
Start-Sleep -Seconds 1
taskkill /IM "$name.exe" /F 2>$null | Out-Null
Start-Sleep -Seconds 2

$found = $null
foreach ($root in @('HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
                    'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall',
                    'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall')) {
    $k = Join-Path $root $appid
    if (Test-Path $k) { $found = (Get-ItemProperty $k).UninstallString; break }
}
if (-not $found) { Write-Output "UNINSTALL: nothing installed for $name"; exit 0 }

$exe = $found.Trim('"')
if (-not (Test-Path $exe)) { Write-Output "UNINSTALL: $exe is gone"; exit 0 }
$p = Start-Process -FilePath $exe -ArgumentList '%s' -PassThru
if (-not $p.WaitForExit(240000)) { $p.Kill(); Write-Output "UNINSTALL: timed out"; exit 1 }
Start-Sleep -Seconds 3
Write-Output "UNINSTALL: $name removed (exit $($p.ExitCode))"
""" % (appid, app_name, WIN_UNSILENT)


def uninstall(mid, also_prod=False):
    """Remove every DEV instance. On `local`, optionally the production one too."""
    m = fleet.MACHINES[mid]
    if m["os"] == "windows":
        script = _ps_uninstall(DEV_APPID, "MoonlightWebDev")
        if also_prod:
            script += "\n" + _ps_uninstall(PROD_APPID, "MoonlightWeb")
        # A bare --dev instance is not an install and has no uninstaller; it is
        # simply a process holding 48080/48443, which the new service needs.
        script += r"""
Get-CimInstance Win32_Process -Filter "Name='MoonlightWeb.exe' OR Name='MoonlightWebDev.exe'" -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Output "KILL: $($_.ProcessId) $($_.CommandLine)"; Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Write-Output "UNINSTALL: done"
"""
        return fleet.run_script(mid, script, timeout=600)

    if m["os"] == "macos":
        pw = m.get("sudo_password", "")
        return fleet.run_script(mid, r"""
caffeinate -d -i -t 300 >/dev/null 2>&1 </dev/null &
launchctl bootout "gui/$(id -u)/com.moonlightweb.agent.dev" 2>/dev/null
rm -f "$HOME/Library/LaunchAgents/com.moonlightweb.agent.dev.plist"
pkill -f MoonlightWebDev 2>/dev/null
sleep 2
echo '%s' | sudo -S -p '' pkgutil --forget com.moonlightweb.server.dev 2>/dev/null
echo '%s' | sudo -S -p '' rm -rf /Applications/MoonlightWebDev.app
echo "UNINSTALL: done"
""" % (pw, pw), timeout=420)

    pw = m.get("sudo_password", "")
    return fleet.run_script(mid, r"""
echo '%s' | sudo -S -p '' systemctl stop moonlightweb-dev.service 2>/dev/null
echo '%s' | sudo -S -p '' systemctl disable moonlightweb-dev.service 2>/dev/null
pkill -f 'MoonlightWebDev|moonlightweb-dev' 2>/dev/null
sleep 2
export DEBIAN_FRONTEND=noninteractive
echo '%s' | sudo -S -p '' apt-get purge -y moonlightweb-dev 2>&1 | tail -3
rm -f "$HOME/.config/autostart/moonlightweb-dev.desktop"
echo "UNINSTALL: done"
""" % (pw, pw, pw), timeout=600)


def install(mid, package_path, label="install"):
    """Put the artifact on the machine and run it silently.

    The package is copied once per machine and kept: the update test runs the
    very same file a second time, which is the point — an update must be the
    same bits arriving twice, not two different builds.
    """
    m = fleet.MACHINES[mid]
    base = os.path.basename(package_path)

    if m["os"] == "windows":
        remote = "C:/Windows/Temp/" + base
        if m["kind"] != "local":
            fleet.push_file(mid, package_path, remote)
        else:
            remote = package_path.replace("\\", "/")
        # Start-Process -Wait would never come back on some of these boxes: the
        # installer launches the app, and the app inherits the handle the shell
        # is waiting on. WaitForExit on the installer alone is bounded and does.
        script = r"""
$pkg = '%s'
if (-not (Test-Path $pkg)) { Write-Output "INSTALL: package missing at $pkg"; exit 1 }
$p = Start-Process -FilePath $pkg -ArgumentList '%s' -PassThru
if (-not $p.WaitForExit(600000)) { $p.Kill(); Write-Output "INSTALL: timed out"; exit 1 }
Write-Output "INSTALL: installer exit $($p.ExitCode)"
Start-Sleep -Seconds 8
$t = Get-ScheduledTask -TaskName 'MoonlightWebDev' -ErrorAction SilentlyContinue
Write-Output ("INSTALL: task " + $(if ($t) { $t.State } else { 'absent' }))

# An ssh session lands in session 0, which has no desktop. The app the
# installer starts at the end of a silent run therefore never comes up, and
# with it never comes up there is no logon task and no server — which reads
# exactly like a broken installer. Starting it through an Interactive
# scheduled task puts it in the console session, where a user's own logon
# would have put it. This is bench scaffolding, not a product step.
$exe = Join-Path $env:ProgramFiles 'MoonlightWebDev\MoonlightWebDev.exe'
if (Test-Path $exe) {
    # Whatever the installer started is a child of THIS ssh session, and
    # Windows tears a session's children down when the session closes: the app
    # logged two healthy minutes and then stopped, at exactly the moment the
    # shell went away. So the copy the installer left is stopped on purpose and
    # the app is started again through an Interactive scheduled task, which
    # belongs to the console session and outlives us. This is bench
    # scaffolding standing in for a user's own logon, not a product step.
    Get-Process -Name 'MoonlightWebDev' -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
    try {
        $who = (Get-CimInstance Win32_ComputerSystem).UserName
        if (-not $who) { $who = "$env:USERDOMAIN\$env:USERNAME" }
        $act = New-ScheduledTaskAction -Execute $exe -Argument '--autostart'
        $pr  = New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
        Register-ScheduledTask -TaskName 'MWBenchLaunch' -Action $act -Principal $pr -Force | Out-Null
        Start-ScheduledTask -TaskName 'MWBenchLaunch'
        # Never Unregister here: deleting a running task kills its process.
        Start-Sleep -Seconds 20
        $running = Get-Process -Name 'MoonlightWebDev' -ErrorAction SilentlyContinue
        Write-Output ("INSTALL: console launch as " + $who + " -> " +
                      $(if ($running) { 'running' } else { 'still not running' }))
    } catch {
        Write-Output ("INSTALL: console launch failed: " + ($_.Exception.Message -replace '"', ''))
    }
    $t2 = Get-ScheduledTask -TaskName 'MoonlightWebDev' -ErrorAction SilentlyContinue
    Write-Output ("INSTALL: logon task " + $(if ($t2) { $t2.State } else { 'absent' }))
}
""" % (remote, WIN_SILENT)
        return fleet.run_script(mid, script, timeout=900)

    if m["os"] == "macos":
        remote = "/tmp/" + base
        fleet.push_file(mid, package_path, remote)
        pw = m.get("sudo_password", "")
        return fleet.run_script(mid, r"""
caffeinate -d -i -t 600 >/dev/null 2>&1 </dev/null &
echo '%s' | sudo -S -p '' installer -pkg '%s' -target / 2>&1 | tail -5
echo "INSTALL: installer rc=$?"
sleep 8
launchctl print "gui/$(id -u)/com.moonlightweb.agent.dev" >/dev/null 2>&1 \
  && echo "INSTALL: agent loaded" || echo "INSTALL: agent absent"
pgrep -f MoonlightWebDev >/dev/null && echo "INSTALL: running" || echo "INSTALL: not running"
""" % (pw, remote), timeout=900)

    remote = "/tmp/" + base
    fleet.push_file(mid, package_path, remote)
    pw = m.get("sudo_password", "")
    return fleet.run_script(mid, r"""
export DEBIAN_FRONTEND=noninteractive
echo '%s' | sudo -S -p '' apt-get install -y --allow-downgrades '%s' 2>&1 | tail -6
echo "INSTALL: apt rc=$?"
sleep 8
echo '%s' | sudo -S -p '' systemctl is-active moonlightweb-dev.service
echo '%s' | sudo -S -p '' systemctl is-enabled moonlightweb-dev.service 2>/dev/null
""" % (pw, remote, pw, pw), timeout=900)


def chapter1(mid, package_path, also_prod=False, log=print):
    """Uninstall, install, then install the same artifact again as an update."""
    step = {"machine": mid, "package": os.path.basename(package_path), "steps": {}}

    log("  [%s] uninstall…" % mid)
    rc, out, err = uninstall(mid, also_prod=also_prod)
    step["steps"]["uninstall"] = {"rc": rc, "out": fleet.redacted(out).strip()[-1500:],
                                  "err": fleet.redacted(err).strip()[-600:]}

    log("  [%s] install…" % mid)
    rc, out, err = install(mid, package_path, "install")
    step["steps"]["install"] = {"rc": rc, "out": fleet.redacted(out).strip()[-1500:],
                                "err": fleet.redacted(err).strip()[-600:]}
    time.sleep(5)
    step["afterInstall"] = fleet.wait_up(mid, timeout=180)

    log("  [%s] update (same artifact again)…" % mid)
    rc, out, err = install(mid, package_path, "update")
    step["steps"]["update"] = {"rc": rc, "out": fleet.redacted(out).strip()[-1500:],
                               "err": fleet.redacted(err).strip()[-600:]}
    time.sleep(5)
    step["afterUpdate"] = fleet.wait_up(mid, timeout=180)

    probe = step["afterUpdate"]
    # Whether the machine has a rendezvous address, and on which server: never the
    # address itself, nor a PIN (fleet.mint_pin, made when a client pairs).
    step["rendezvousKnown"] = bool((probe.get("internet") or {}).get("rendezvousKnown"))
    step["rendezvousHost"] = fleet.rendezvous_host(probe)
    step["lanUrl"] = fleet.lan_url(mid, probe)
    step["ports"] = {"http": probe.get("httpPort"), "https": probe.get("httpsPort"),
                     "listening": probe.get("listening")}
    return step


if __name__ == "__main__":
    import sys
    if len(sys.argv) < 3:
        raise SystemExit("usage: install.py <machine> <package> [--also-prod]")
    result = chapter1(sys.argv[1], sys.argv[2], also_prod="--also-prod" in sys.argv)
    print(json.dumps(result, indent=2))


def cleanup_launch_task(mid):
    """Remove the bench's Interactive launch task, once nothing needs it.

    Deleting it while the app is still running would kill the app, so this is
    called at the very end of a campaign, after the last pass.
    """
    if fleet.MACHINES[mid]["os"] != "windows":
        return 0, "", ""
    return fleet.run_script(mid, r"""
Unregister-ScheduledTask -TaskName 'MWBenchLaunch' -Confirm:$false -ErrorAction SilentlyContinue
Write-Output 'CLEANUP: MWBenchLaunch removed if it existed'
""", timeout=120)
