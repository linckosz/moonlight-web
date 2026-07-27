[← REST API](08-REST-API.md) · **Installers & Packaging** · [Next: PowerDNS Stack →](10-PowerDNS-Stack.md)

---

# 9. Installers & Packaging

Each platform gets a native, double-clickable installer with the same goal: a non-technical user finishes with a running server, an authorized (or declined) Internet link, and a paired local Sunshine. Everything ships from the `release.yml` GitHub Actions workflow (see [Build & CI](11-Build-CI-Testing.md)).

## 9.1 Windows — Inno Setup (`backend/installer/moonlightweb.iss`)

**Artifact**: `MoonlightWeb-installer-<version>-win-{x64,arm64}.exe` (Inno Setup 6, LZMA2, admin privileges, EN/FR/zh-CN — the Chinese translation is vendored as `ChineseSimplified.isl`).

**Wizard flow**:

1. Install the app + Start-Menu shortcuts (app, admin page, uninstaller). The Desktop/Start-Menu shortcuts are **`.lnk` files pointing at the exe** — launching the exe starts the server when down, or surfaces the admin page via the single-instance logic when up.
2. **Internet Link page** — explicit opt-in checkbox (unchecked by default) to publish the public domain.
3. **Sunshine page** — detects an existing Sunshine; offers a silent install (downloads the arch-specific NSIS installer from LizardByte's latest release, runs `/S`); collects Sunshine credentials for auto-pairing (skippable).
4. Drops a **`provisioning.json`** next to the exe; the server consumes it once on first run (`Provisioning::applyOnce`): enable Internet Access (with the consent recorded as `source:"installer"`), create/pair the local Sunshine via its REST API (`/api/pin`).
5. Runs the server during a **live provisioning checklist** page (Sunshine install → pairing → A-record published — the async A-record step is fed back by the server through a status file), then offers to open the admin page (the URL the server actually published, real port/domain).

An optional logon task (`--autostart`) starts the server at login without opening a browser. **No DNS/ACME secrets ship in the installer** — they are compiled into the exe by CI.

**Update mode**: when a MoonlightWeb is already installed (uninstall key or a leftover exe under `{app}`), the wizard collapses to a single confirmation page — button **Update**, a one-line "x.y.z → a.b.c" memo, everything else skipped (`ShouldSkipPage`). The rule for skipping is *"was this question already answered?"*, read from `settings.json`, so a page added by a future version still shows up on an update for free. The Sunshine page is always skipped (pairing lives on the admin page from then on), the shortcut/autostart tasks are re-derived from what is actually on the machine instead of falling back to their defaults, `provisioning.json` is not rewritten, and the post-install checklist does not run. The running server is stopped before the file copy (service `net stop`, else logon task `/End` + `taskkill`) and restarted after.

**Elevated update launcher**: every install registers a trigger-less scheduled task `MoonlightWeb Update` (`RunLevel=HighestAvailable`) whose action is `%LocalAppData%\MoonlightWeb\update\MoonlightWeb-update.exe /VERYSILENT …`. The server runs unprivileged, so this is what lets it apply an update with **no UAC prompt** — one that a browser on another continent could never answer (see §9.4).

**Uninstall**: stops the server (logon task `/End` + `/Delete`, `taskkill`), drops the `MoonlightWeb Update` task and its staging dir, the firewall rule, the shortcuts and `provisioning*.json`. Everything the user configured lives outside `{app}` and is **kept by default**; a modal shown at `usUninstall` offers an **unchecked** "also delete my configuration" box which, when ticked, wipes `%AppData%\MoonlightWeb\MoonlightWeb` (settings, sessions, ACME certificate, logs, crash dumps) and `HKCU\Software\MoonlightWeb` (paired hosts + Moonlight client identity). A silent uninstall never asks and never deletes.

**Service option**: `backend/packaging/windows/install-service.bat` / `uninstall-service.bat` wrap NSSM for a session-0 service install (sets `MW_SERVICE`).

## 9.2 macOS — interactive `.pkg` (`backend/installer/macos/`)

**Artifact**: `moonlightweb-macos-arm64.pkg` built by `build-pkg.sh`:

1. Compiles an **Installer.app plugin** (`MWSunshinePane.m` + xib): a custom wizard pane inside the native macOS installer that handles the Sunshine step. The deprecated `InstallerPlugins.framework` is header-stripped in the SDK, so the pane API is **self-declared** (`MWInstallerPane.h`) and only the framework binary is linked — do *not* add `-F /System/Library/Frameworks` (it shadows the SDK). `InstallerSections.plist` must sit beside the bundle.
2. `pkgbuild` packs `MoonlightWeb.app` (+ `postinstall` script) into a component package installed to `/Applications`.
3. `productbuild` wraps it with `distribution.xml` + HTML resources (welcome/license/conclusion) into the Introduction → License → Sunshine → Install → Summary flow.

TCC permissions (Screen Recording for Sunshine) cannot be granted programmatically — the app exposes `/api/system/open-screen-recording` to open the right Settings pane, and the in-app **SetupView wizard remains the fallback** for anything the pkg couldn't do. A LaunchAgent (`backend/packaging/launchd/com.moonlightweb.server.plist`) provides start-at-login; the Desktop gets a `.url` convenience shortcut.

## 9.3 Linux — `.deb` / `.rpm` / AppImage (`backend/packaging/linux/make-packages.sh`)

One **linuxdeploy AppDir** (binary + bundled Qt runtime with `$ORIGIN` rpaths + frontend) is built in CI, then:

- **AppImage** for Arch-based and immutable distros (SteamOS, Bazzite).
- **`.deb` + `.rpm`** via **fpm**: the AppDir tree is relocated under `/opt/moonlightweb`, with a `qt.conf`, a `/usr/bin/moonlightweb` symlink, a hicolor icon and a `.desktop` menu entry (absolute `Exec=`, `/opt` isn't on PATH). **No hard dependencies** — Qt/OpenSSL are bundled; naming distro libs would make the package distro-specific.
- **postinst**: refreshes desktop/icon caches; **opens firewall ports best-effort** (443/tcp, 80/tcp, 47999/udp on firewalld/ufw — Linux firewalls are port-based, and this runs before the app picks a port); **launches the app inside the active graphical session** via `systemd-run --user` (postinst runs as root with no display).
- **prerm**: stops the running instance; removes the firewall rules on uninstall (kept on upgrade).

Autostart uses an XDG autostart entry; a systemd unit (`backend/packaging/systemd/moonlightweb.service`) covers headless/service installs. The Desktop shortcut is a `Type=Application` entry executing the binary (a `Type=Link` renders a generic icon and GNOME refuses to launch it) marked `gio trusted`.

## 9.4 Shared runtime behaviors

| Behavior | Mechanism |
|---|---|
| Single instance | `QLockFile`; a second launch focuses/opens the admin page then exits 0. |
| Auto-restart on crash | systemd `Restart=on-failure` / launchd KeepAlive / NSSM; **exit 0 = voluntary quit, never restarted**. |
| Self-healing shortcuts | The server rewrites the Desktop entry on startup and whenever the entry URL changes (port parity rebind, Internet Access ready) — the installer can't know the runtime port/domain. |
| Auto-update discovery | `UpdateChecker` → GitHub Releases (`/api/update/check`), a discreet banner on the hosts page. It never offers a download: the installer must run on the **host**, not on the phone reading the banner. |
| Auto-update application | `SelfUpdater` (`/api/update/start`) downloads the resolved asset and runs it unattended. The banner only offers the one-click path when the host machine is **paired with its local Sunshine** — the marker that the installer has nothing left to ask — otherwise it just says "update the host PC". |
| Browser auto-open | Manual GUI launches open `/setup` (first run, macOS/Linux) or `/admin`; `--autostart`/headless launches stay silent. |

## 9.5 Workarounds catalog (installers)

| Problem | Workaround |
|---|---|
| Ubuntu snap-Firefox opens a blank window via the XDG portal | `xdg-open` invoked directly on Linux (`openInBrowser`). |
| GNOME desktop icons refuse to launch untrusted entries | Entry made executable + `gio set metadata::trusted true`. |
| Program Files not writable by the user session | Logs/dumps/settings live in the per-user data dir, never next to the exe. |
| xcaddy/Go OOM, small VMs (DNS box) | (See PowerDNS chapter — swap added by its installer.) |
| macOS Installer plugin API removed from SDK headers | Self-declared headers + link-only framework (see §9.2). |
| Windows service session 0 has no desktop | `MW_SERVICE` suppresses tray/shortcut/browser behaviors. |
| A remote browser cannot answer a UAC / polkit / `osascript` prompt on the host desktop | Windows: elevated trigger-less scheduled task started with `schtasks /Run` (§9.1). Linux AppImage: in-place file replacement, no root at all. Service/root installs: already elevated. Everything else reports `requires_host_confirmation` so the UI says "confirmation required on the host PC" instead of hanging on an invisible dialog. |
| An elevated task running an exe from a writable directory is a privilege-escalation hole | Staging lives under per-user `%LocalAppData%` (expanded by Task Scheduler in the task's own user context), never `%ProgramData%`. |
| Inno Setup can't know the final admin URL | Server publishes `admin_url` via `Provisioning::setInfo`; installer reads it post-install with a provisional fallback. |

---

[← REST API](08-REST-API.md) · [Home](Home.md) · [Next: PowerDNS Stack →](10-PowerDNS-Stack.md)
