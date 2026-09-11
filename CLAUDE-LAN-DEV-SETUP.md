# LAN-only development environment — setup plan for Claude Code

> **Who reads this:** Claude Code running **Claude Opus**, started at the root of a **fork** of
> [`linckosz/moonlight-web`](https://github.com/linckosz/moonlight-web). A human can follow it too.
>
> **How it is triggered:** the user types
> *“Read CLAUDE-LAN-DEV-SETUP.md and set up my LAN-only dev environment.”*
>
> **What you deliver:** a machine on which the user can change MoonlightWeb, run it on their LAN,
> pass the same gates as CI and open a pull request — and on which the short prompts of
> [§14](#14-short-prompts-for-the-everyday-loop) work from then on.

---

## 0. Hard constraints

These are not preferences. If a step would break one, stop and tell the user.

1. **LAN only, by IP.** The instance is reached at `https://localhost:<port>` and
   `https://<LAN-IP>:<port>` with its own self-signed certificate. No domain, no DNS record, no
   certificate request, no Internet URL.
2. **No contact with the project's infrastructure** — the rendezvous server
   (`stream.moonlightweb.top`, its staging `stream.dev.moonlightweb.top`), the DNS stack (PowerDNS,
   `deploy/powerdns/`), the `updates.` and `metrics.` endpoints. The code enforces it with
   `MW_LAN_ONLY=1` (below). Never unset it or set it to `0`; never set `MW_DOMAIN`,
   `MW_RENDEZVOUS_URL` or `MW_PDNS_TOKEN`; never run anything under `deploy/` or `bootstrap/`; do
   not even resolve those host names to “check” something.
3. **No virtual machine.** No Hyper-V, no WSL, no Docker, no VirtualBox: the user may be on
   **Windows 11 Home** with a modest CPU. Everything installs and runs on the host.
4. **The user's other MoonlightWeb stays untouched.** If an official MoonlightWeb is installed,
   never stop, update or reconfigure it. The development instance always runs with `--dev`: its
   own state, its own ports, no single-instance lock.

### What `MW_LAN_ONLY=1` does

| Area | On a LAN-only instance |
|---|---|
| Internet Access | reads as **off** whatever `settings.json` says; turning it on is **refused with an error** — admin page toast, setup wizard, `POST /api/internet/enable` (409, nothing written), `--enable-internet` (exit 1) |
| Rendezvous | the line never opens; no rendezvous address is logged, shown or handed out (tray, share links) |
| STUN | no STUN server is given to the browser or to the host's WebRTC stack — host candidates only |
| UPnP | no router discovery, no port mapping (`GET /api/internet/upnp-probe` → 409) |
| Update check, census | not made |
| `--dev` | does not point at the staging rendezvous |
| Log | `[LAN-only] MW_LAN_ONLY is set: …` at startup, and `stun_server=(none, LAN-only)` |

Streaming on the LAN, pairing Sunshine / Wolf hosts on the LAN, session sharing between LAN
devices: unchanged.

`MW_LAN_ONLY` is read from the environment — the checkout's `.env` included — and, when the build
is configured with it in the environment, compiled into the binary as a fallback (§5). The code is
`mw::edition::lanOnly()` in `backend/src/common/Edition.cpp`; the reference is
[`docs/wiki/07-Settings-Reference.md`](docs/wiki/07-Settings-Reference.md#73-env--environment-configuration).

---

## 1. Rules for you (Claude)

- **Ask before installing anything**, and before anything that needs administrator rights (a UAC
  prompt the user has to accept). Show the exact command first.
- **Survey first** (§2). Skip every step whose result is already there; never reinstall a working
  tool.
- Windows commands are **PowerShell** unless marked *Git Bash*, run from the repository root.
  Shell state does not persist between your tool calls: set `$env:QTDIR` and friends in the same
  command that needs them.
- A shell that was already running does not see a `PATH` changed by `winget`. Refresh it in the
  command that needs the new tool:
  `$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')`
- A refused Internet Access request is the **expected** behaviour here. Never “fix” it.
- Never commit `.env`, a build directory, a log or a submodule pointer. Stage files by explicit
  path — never `git add -A` or `git add .`.
- Never modify `backend/third_party/**` or `backend/native-host/third_party/**` (submodules).
- When a step fails, read §13 before improvising, and report the exact error line.
- Say one short line per step done. Answer in the user's language.

---

## 2. Survey the machine (read-only)

```powershell
Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, OSArchitecture
Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors
"RAM GB: " + [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion
Get-PSDrive -PSProvider FileSystem | Select-Object Name, @{n = 'FreeGB'; e = { [math]::Round($_.Free / 1GB, 1) } }
foreach ($t in 'git', 'gh', 'py', 'python', 'node', 'npm', 'cmake', 'ninja') {
    $c = Get-Command $t -ErrorAction SilentlyContinue
    '{0,-7} {1}' -f $t, $(if ($c) { $c.Source } else { '-- missing' })
}
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Get-ChildItem C:\Qt -Directory -ErrorAction SilentlyContinue | Select-Object Name
Get-Service MoonlightWeb -ErrorAction SilentlyContinue
Get-Process MoonlightWeb -ErrorAction SilentlyContinue | Select-Object Id, Path
```

Then tell the user, in a few lines:

- **OS** — x64 Windows 10 or 11, Home or Pro: both fine. **Windows on ARM: stop here** — this plan
  targets x64 (the native engine does not run on ARM64, and that build takes its OpenSSL from
  vcpkg; see `.github/workflows/release.yml`).
- **Disk** — about **15 GB** free: Build Tools ~7 GB, Qt ~1.5 GB, sources and build ~3 GB, Node
  modules ~0.5 GB.
- **RAM / cores** — under 8 GB of RAM or under 4 cores: build with `BUILD_JOBS=2` (§5). A first
  build takes roughly 10 minutes on a fast desktop, 30–60 on a small laptop; later ones take
  seconds to minutes.
- **GPU** — NVIDIA / AMD / Intel with a recent driver: hardware encoding. No usable GPU encoder: the
  native engine falls back to software H.264 — it works; stream at 720p / 30 fps.
- **What is missing**, and whether an official MoonlightWeb is installed (it will be left alone).

---

## 3. Fork, clone, submodules

If the current directory is already a clone, check `git remote -v`: `origin` must be **the user's
fork**. If there is no clone yet:

```powershell
# With the GitHub CLI (winget install --id GitHub.cli -e, then: gh auth login).
# It forks, clones, and adds the original repository as the `upstream` remote.
gh repo fork linckosz/moonlight-web --clone
# …or fork on github.com, then: git clone https://github.com/<user>/moonlight-web.git
```

Then, inside the clone:

```powershell
git remote add upstream https://github.com/linckosz/moonlight-web.git   # skip if it exists
git config core.longpaths true
git submodule update --init --recursive
```

`.gitmodules` sets `ignore = all`: `git status` never shows the submodules, so a changed submodule
pointer is invisible there — §11 checks for one before a PR.

---

## 4. Toolchain (Windows)

Install only what §2 found missing. `winget` may show one UAC prompt per package.

| # | Tool | Why | Install |
|---|---|---|---|
| 1 | **Git for Windows** (with Git Bash) | clone, submodules; Git Bash runs `scripts/run-tests.sh` | `winget install --id Git.Git -e` |
| 2 | **Visual Studio 2022 Build Tools** — MSVC v143, Windows SDK, CMake, Ninja | the C++ compiler | below |
| 3 | **Python 3** | installs Qt (aqtinstall) and clang-format | `winget install --id Python.Python.3.12 -e` |
| 4 | **Node.js LTS** (CI uses 22) | frontend lint, format, tests | `winget install --id OpenJS.NodeJS.LTS -e` |
| 5 | **Qt 6.11.0** — MSVC 2022 64-bit + **Qt WebSockets** | the framework | below |
| 6 | **clang-format 19.1.7** (CI's exact version) | the C++ formatting gate | `py -m pip install --user clang-format==19.1.7` |
| 7 | *optional* **GitHub CLI** | fork, PR, CI logs | `winget install --id GitHub.cli -e` |
| 8 | *optional* **OpenCppCoverage** | the backend coverage % (CI enforces 70 %) | `winget install --id OpenCppCoverage.OpenCppCoverage -e` |

**Build Tools** — the free package without the IDE. An existing Visual Studio 2022 or newer with
the *Desktop development with C++* workload works too: the build scripts find it with `vswhere`.

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.VC.CMake.Project --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --includeRecommended"
```

**Qt**, without a Qt account and exactly as CI installs it: aqtinstall pinned to the commit that
understands Qt 6.11's repository layout (every release up to 3.3.0 fails on it with 404s), and the
official download server:

```powershell
py -m pip install --user "aqtinstall @ git+https://github.com/miurahr/aqtinstall.git@8c3695d4a4e1ceabf6a74dc6c79681656dc6b74b" "py7zr==1.1.0"
py -m aqt install-qt windows desktop 6.11.0 win64_msvc2022_64 -m qtwebsockets -O C:\Qt --base https://download.qt.io
Test-Path C:\Qt\6.11.0\msvc2022_64\lib\cmake\Qt6WebSockets\Qt6WebSocketsConfig.cmake   # must be True
```

Fallback: the [Qt online installer](https://www.qt.io/download-qt-installer-oss) (free Qt account)
→ *Qt 6.11.0 → MSVC 2022 64-bit* and *Qt WebSockets*.

Check everything:

```powershell
git --version; py --version; node --version; npm --version
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cf = py -c "import clang_format, os; print(os.path.join(os.path.dirname(clang_format.__file__), 'data', 'bin', 'clang-format.exe'))"
& $cf --version   # clang-format version 19.1.7
```

---

## 5. Make it LAN-only, then build

Create `.env` at the repository root with exactly this. Never copy `.env.example`: its placeholder
`MW_DOMAIN` / `MW_PDNS_TOKEN` values would point the instance at servers.

```powershell
Set-Content -Path .env -Encoding ascii -Value "# Local only, never committed. See CLAUDE-LAN-DEV-SETUP.md.`r`nMW_LAN_ONLY=1"
git check-ignore .env   # must print: .env
```

The server reads `.env` next to its executable, or else at the root of the checkout it was built
from — so every build of this checkout picks it up.

Build with `MW_LAN_ONLY` in the environment too, so it is **compiled into the binary** and a copy of
the exe run from elsewhere stays LAN-only:

```powershell
$env:QTDIR = 'C:\Qt\6.11.0\msvc2022_64'   # explicit: the script otherwise takes the highest C:\Qt\6.* kit
$env:MW_LAN_ONLY = '1'
# $env:BUILD_JOBS = '2'                   # only on a small machine (§2)
cmd /c backend\build_msvc.bat
```

Expect `MW_LAN_ONLY embedded at build time` in the configure output and
`[OK] Built: …\build\MoonlightWeb.exe` at the end. Every later build uses the same lines: the
script reconfigures each time, and a configure without `MW_LAN_ONLY` in its environment drops the
compiled-in value (the `.env` still covers runs from this checkout).

Then, **once**, put the Qt runtime next to the binary — CMake only copies the OpenSSL DLLs, and the
release pipeline is what normally runs `windeployqt`:

```powershell
$env:QTDIR = 'C:\Qt\6.11.0\msvc2022_64'
& "$env:QTDIR\bin\windeployqt.exe" --release --no-translations --no-opengl-sw build\MoonlightWeb.exe
New-Item -ItemType Directory -Force build\tls | Out-Null
Copy-Item "$env:QTDIR\plugins\tls\qopensslbackend.dll" build\tls\ -Force
```

The last two lines matter: `windeployqt` never ships Qt's OpenSSL TLS backend, and without it Qt
falls back to Schannel, which breaks the loopback HTTPS the command-line tools (`--status`,
`--new-pin`) rely on.

The web UI is served straight from the checkout's `frontend/` folder (its path is compiled in):
JavaScript and CSS changes need no rebuild.

---

## 6. Firewall (once)

So that a phone or another PC on the LAN can reach the instance (one UAC prompt):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\dev-firewall-allow.ps1
```

It adds port rules on every profile for TCP 48080 / 48443 and TCP + UDP 48010–48033 — the `--dev`
web ports and the per-stream WebRTC media ports. Also ask the user to check that the PC's network
is **Private** (*Settings → Network & internet → Wi-Fi or Ethernet → Network profile type*) and that
the other device is on the same network, not on a guest Wi-Fi that isolates clients.

---

## 7. Run and prove it is LAN-only

Start the development instance detached, logging into the git-ignored build folder. It opens the
admin page in the default browser by itself.

```powershell
Start-Process -FilePath build\MoonlightWeb.exe -ArgumentList '--dev', '--log', "$PWD\build\dev.log"
Start-Sleep -Seconds 10
Select-String -Path build\dev.log -Pattern 'Settings:|Server ready|From another machine|From the internet|\[RDV\]'
Select-String -Path "$env:APPDATA\MoonlightWeb\MoonlightWeb-dev\logs\moonlightweb.log" -Pattern 'LAN-only' | Select-Object -Last 1
```

Lines written before the command line is parsed — the `[LAN-only]` one among them — go to the
default log, hence the second file. Expected:

- `[LAN-only] MW_LAN_ONLY is set: no rendezvous line, no STUN, no UPnP, no update check — Internet Access requests are refused`
- `[main] Settings: … stun_server=(none, LAN-only)`
- `Server ready. Open https://localhost:48443 in your browser.`, then one
  `From another machine on this network: https://<ip>:48443` line per network adapter. The LAN one
  is usually `192.168.x.x` or `10.x.x.x`; `172.x.x.x` and `192.168.56.x` belong to Hyper-V,
  VirtualBox or WSL adapters.
- **No** `From the internet:` line and **no** `[RDV]` line.

48443 is the port of a fresh `--dev` state; if the log names another, use that one below.

Prove the refusal through the API — with the same admin-key handshake the CLI uses, and in Python
because it accepts the self-signed certificate on loopback:

```powershell
@'
import json, ssl, sys, urllib.request
base = "https://127.0.0.1:" + (sys.argv[1] if len(sys.argv) > 1 else "48443")
ctx = ssl._create_unverified_context()
def call(method, path, body=None, key=""):
    headers = {"Content-Type": "application/json"}
    if key:
        headers["X-MW-Admin-Key"] = key
    req = urllib.request.Request(base + path, method=method, headers=headers,
                                 data=None if body is None else json.dumps(body).encode())
    try:
        with urllib.request.urlopen(req, context=ctx, timeout=15) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()
key = json.loads(call("GET", "/api/admin/token")[1]).get("token", "")
status = json.loads(call("GET", "/api/internet/status")[1])
print("lan_only :", status.get("lan_only"), "| rendezvous:", status.get("rendezvous"))
print("enable   :", *call("POST", "/api/internet/enable", {"internet_access_enabled": True}, key))
print("refresh  :", *call("POST", "/api/internet/refresh", {}, key))
print("upnp     :", *call("GET", "/api/internet/upnp-probe"))
'@ | py - 48443
```

Expected: `lan_only : True`, a rendezvous `url` that is empty, and **409**
`This instance is LAN-only (MW_LAN_ONLY is set): Internet access cannot be enabled…` for `enable`,
`refresh` and `upnp`.

And prove that nothing leaves the LAN — this must print nothing:

```powershell
$ids = (Get-CimInstance Win32_Process -Filter "Name='MoonlightWeb.exe'" | Where-Object ExecutablePath -like "$PWD\build\*").ProcessId
Get-NetTCPConnection -OwningProcess $ids -ErrorAction SilentlyContinue |
    Where-Object { $_.State -ne 'Listen' -and $_.RemoteAddress -notmatch '^(0\.0\.0\.0|127\.|10\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.|::1?$|fe80:)' }
```

If any of these checks fails, stop: fix §5 before going further.

Now hand over to the user:

1. Open **https://localhost:48443** on the PC and accept the certificate warning
   (*Advanced → Continue*): it is the instance's own self-signed certificate.
2. The PC is already in the list as **`<hostname> — MoonlightWeb Host`**. The admin page is
   `/admin`, reachable from this PC only.
3. From a phone or another PC: **https://\<LAN-IP\>:48443**. If it asks for a PIN,
   `build\MoonlightWeb.exe --dev --new-pin` prints one (single-use: one per device).
4. Stream from the *other* device — streaming a screen to a browser on that same screen works, but
   shows an endless mirror.
5. Optional: tick *Internet access* on the admin page. The toast
   *“Failed to enable: This instance is LAN-only…”* is the expected answer.

No host card? `https://localhost:48443/api/native/status` gives the reason (no usable encoder, no
display…). Hosts paired on the LAN (Sunshine, Wolf) keep working either way.

To stop the instance — this checkout's build only, never an installed MoonlightWeb:

```powershell
Get-CimInstance Win32_Process -Filter "Name='MoonlightWeb.exe'" | Where-Object ExecutablePath -like "$PWD\build\*" | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
```

---

## 8. Baseline: run the gates once

Prove the environment before the user changes anything:

```powershell
cd frontend; npm ci; npm run check; npm test; cd ..
$env:QTDIR = 'C:\Qt\6.11.0\msvc2022_64'; cmd /c backend\tests\run_coverage.bat
```

- `npm run check` is Prettier + ESLint + the i18n key check — CI's frontend gate; `npm test` is
  Vitest.
- `run_coverage.bat` builds and runs the Qt test runner (`Backend TNR: N/N checks passed`), then the
  coverage gate when OpenCppCoverage is installed. It rebuilds `backend\tests\build` from scratch
  every time (a few minutes).
- *Git Bash*, both suites with coverage in one go: `bash scripts/run-tests.sh`.

A failure on an untouched checkout is not the user's doing: report it with its output, don't patch
around it.

Finish the setup with a summary: what was installed, the two URLs (localhost and LAN IP), the
LAN-only proof, the gate results, and a pointer to the prompts of §14.

---

## 9. The everyday loop

- **Frontend change** (`frontend/`): reload the page with `Ctrl+F5`. Still the old code? DevTools →
  *Application* → *Storage* → *Clear site data*, then reload.
- **Backend change** (`backend/`): stop the instance (a running `build\MoonlightWeb.exe` is locked:
  `LNK1104`), rebuild with the §5 environment, relaunch (§7). The Qt DLLs stay in place.
- **Logs**: `build\dev.log`, and the early lines in
  `%APPDATA%\MoonlightWeb\MoonlightWeb-dev\logs\moonlightweb.log`.
- **Before touching an area**, read its chapter in [`docs/wiki/`](docs/wiki/Home.md) —
  [`05-Streaming-and-Transports.md`](docs/wiki/05-Streaming-and-Transports.md) in particular lists
  fixes that look like bugs.
- **House style**: C++17 / Qt 6; vanilla ES6 modules on the frontend — no framework, no build step,
  no new dependency without asking; code comments in English, 1–2 lines, explaining constraints
  rather than mechanics.

---

## 10. Formatting

**Frontend:**

```powershell
cd frontend; npm run format:fix; npm run check; cd ..
```

**C++** — only the files you changed, with CI's clang-format 19.1.7 and the house style
`backend/.clang-format`:

```powershell
git fetch upstream
$cf = py -c "import clang_format, os; print(os.path.join(os.path.dirname(clang_format.__file__), 'data', 'bin', 'clang-format.exe'))"
$files = git diff --name-only upstream/main -- backend/src backend/tests | Where-Object { $_ -match '\.(cpp|h)$' -and (Test-Path $_) }
if ($files) { & $cf --dry-run --Werror $files }   # report only
if ($files) { & $cf -i $files }                   # rewrite in place
git diff --stat
```

After `-i`, read the diff and keep only hunks on lines you wrote. The Windows build of clang-format
counts a non-ASCII character — the `—` common in this codebase's comments — as several columns, so
it may re-wrap an untouched line that CI (Linux) accepts: restore such a line by hand.

---

## 11. Preparing a pull request

1. **Start from a fresh upstream:**

   ```powershell
   git fetch upstream
   git switch main; git merge --ff-only upstream/main; git push origin main
   git switch -c feat/<short-name>
   ```

2. **Commit** in English with [Conventional Commits](https://www.conventionalcommits.org/) —
   `feat(scope): …`, `fix(scope): …`, `docs: …`, `test: …`; `git log` shows the style. Keep a PR on
   one domain (backend **or** frontend **or** docs/config). New UI text goes through i18n, with its
   key in every locale file — `npm run check` fails otherwise.
3. **Gates**: §10, `npm run check` and `npm test`, `run_coverage.bat` (or
   `bash scripts/run-tests.sh`). All green before pushing.
4. **Check what the PR contains:**

   ```powershell
   git fetch upstream
   git log --oneline upstream/main..HEAD
   git diff --stat upstream/main...HEAD
   git diff upstream/main...HEAD --submodule=short | Select-String '^[-+]Subproject'   # must print nothing
   ```

   Nothing from `.env`, `build*/`, logs or `node_modules/`, and no submodule line.
5. **Rebase** if upstream moved: `git fetch upstream; git rebase upstream/main`, run the gates
   again, then `git push --force-with-lease origin HEAD` — on your own branch only.
6. **Open the PR** against `linckosz/moonlight-web`, branch `main`:

   ```powershell
   git push -u origin HEAD
   gh pr create --repo linckosz/moonlight-web --base main --title "feat(scope): …" --body "<what, why, how it was tested>"
   ```

   The body says how it was tested — the LAN-only instance, which devices and browsers — with a
   screenshot for any UI change. CI on a fork's PR runs the frontend gate, clang-format, cppcheck
   and the backend tests; packaging is skipped for forks. `gh pr checks` follows it.

Draft commit messages and the PR title and body, and **show them to the user before committing or
opening the PR**.

---

## 12. Linux and macOS — what differs

Everything above holds, with these substitutions:

| | Linux (Debian / Ubuntu) | macOS (Apple Silicon) |
|---|---|---|
| Toolchain | `sudo apt install build-essential cmake ninja-build pkg-config libssl-dev git python3-venv` + Node 22 (nodejs.org or NodeSource) | `xcode-select --install`, then `brew install cmake ninja openssl@3 pkg-config node@22 python` |
| Python tools | distributions that refuse `pip install --user` (PEP 668): `python3 -m venv ~/.venvs/mw && . ~/.venvs/mw/bin/activate`, then the §4 `pip install` lines with `pip` | same, if pip refuses |
| Qt (same pinned aqtinstall) | `python -m aqt install-qt linux desktop 6.11.0 linux_gcc_64 -m qtwebsockets -O ~/Qt --base https://download.qt.io` | `python -m aqt install-qt mac desktop 6.11.0 clang_64 -m qtwebsockets -O ~/Qt --base https://download.qt.io` |
| `.env` | `printf 'MW_LAN_ONLY=1\n' > .env` | same |
| Build | `MW_LAN_ONLY=1 CMAKE_PREFIX_PATH=<kit> ./backend/build.sh`, `<kit>` being the folder aqt created under `~/Qt/6.11.0/` (`gcc_64`) | same (`macos`) |
| Run | `./build/MoonlightWeb --dev --log build/dev.log` | same |
| Firewall | if `ufw` is active: `sudo ufw allow 48080,48443/tcp` and `sudo ufw allow 48010:48033/udp` | allow the binary when macOS asks |
| Own screen | the installed packages grant the capability KMS capture needs (`backend/packaging/linux/make-packages.sh`); a plain build may fall back to the ScreenCast portal, or show no host card | grant *Screen Recording* to the binary at first launch |
| Backend tests | CI runs the Qt suite on Windows; locally, rely on the PR's CI for it | same |

The LAN-only proof of §7 works unchanged with `python3` in place of `py` and the connection check
replaced by `ss -tnp | grep MoonlightWeb` (Linux) or `lsof -nP -iTCP -a -c MoonlightWeb` (macOS).

---

## 13. Troubleshooting

| Symptom | Cause → fix |
|---|---|
| `Qt 6.11 (MSVC 2022 64-bit) was not found`, or CMake cannot find `Qt6WebSockets` | `$env:QTDIR` unset, or a kit without WebSockets → point it at the §4 kit |
| `aqt` 404s, *Updates.xml* errors | an aqtinstall release instead of the pinned commit, or a mirror → the §4 commands, `--base https://download.qt.io` kept |
| `Visual Studio 2022 with the C++ toolset was not found` | Build Tools without the C++ workload → the §4 Build Tools command |
| `fatal error C1060: compiler is out of heap space`, or the PC freezes while building | too many compilers at once → `$env:BUILD_JOBS = '2'` (or `'1'`) |
| `LNK1104: cannot open file '…MoonlightWeb.exe'` | the dev instance is running → stop it (§7) |
| `Qt6Core.dll was not found` at launch | Qt runtime not deployed → `windeployqt` (§5) |
| Log: `OpenSSL TLS backend unavailable … using schannel`; `--new-pin` fails | `build\tls\qopensslbackend.dll` missing → §5 |
| `Another instance is already running` | launched without `--dev` while an installed MoonlightWeb runs → always `--dev` |
| No `[LAN-only]` line, or a `From the internet:` line | `.env` missing or wrong, or the exe was copied away from a build configured without `MW_LAN_ONLY` → §5. Stop until fixed |
| The phone cannot open `https://<ip>:48443` | firewall script not run, network profile *Public*, guest / isolated Wi-Fi, or a VM adapter's IP → §6, the LAN IP |
| Certificate warning in the browser | expected: self-signed on the LAN |
| *“Failed to enable: This instance is LAN-only…”* | expected — that is the point |
| No `— MoonlightWeb Host` card | `/api/native/status` names the reason (no usable encoder, no display, ARM64) |
| A tool installed by `winget` is “not recognized” | stale `PATH` in the running shell → the refresh line of §1 |
| `npm run check` fails on files you did not touch | tool versions differ from the lockfile → `npm ci`, not `npm install` |
| clang-format wants to change lines you did not touch | Windows column counting (§10) → leave those lines alone |

---

## 14. Short prompts for the everyday loop

Once the setup is done, the user drives the environment with these. Each maps to steps above:
carry it out under the §0 constraints and the §1 rules, and show any commit message or PR text
before using it.

| Prompt | What Claude does |
|---|---|
| **Set up my LAN-only dev environment.** | §2 to §8 of this file |
| **Rebuild and relaunch.** | stop the `--dev` instance started from `build\`, build with the §5 environment, relaunch, show the `Server ready` lines |
| **Relaunch.** | stop and start the instance without building |
| **Stop the dev instance.** | the §7 stop command — this checkout's exe only |
| **Is it still LAN-only?** | the §7 proof: log lines, the API 409s, no connection outside the LAN |
| **Give me a PIN for my phone.** | `build\MoonlightWeb.exe --dev --new-pin`, plus the LAN URL |
| **Show me the dev log.** | the last lines of both logs, warnings and errors first |
| **Run the frontend checks.** | `npm run check` and `npm test` |
| **Run the backend tests.** | `run_coverage.bat` with `QTDIR` set |
| **Format my changes.** | §10, frontend and C++, changed files only |
| **Run the full PR gate.** | §10 in check mode, `npm run check`, `npm test`, the backend tests — one pass / fail summary |
| **Sync with upstream.** | fetch `upstream`, fast-forward `main`, rebase the current branch on it |
| **Start a feature branch *name*.** | sync, then `git switch -c feat/<name>` |
| **Commit my work.** | `git status`, stage explicit paths, draft a Conventional Commit in English — shown first |
| **What will my PR contain?** | §11 step 4 |
| **Open the PR.** | the full gate, push, draft title and body — shown first — then `gh pr create` |
| **Why did CI fail on my PR?** | `gh pr checks`, `gh run view <id> --log-failed`, reproduce locally, propose a fix |
| **Check my toolchain.** | re-run §2 and report any drift from CI (Qt 6.11.0, clang-format 19.1.7, Node 22) |
