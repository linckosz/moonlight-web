[← REST API](08-REST-API.md) · **Installers & Packaging** · [Next: PowerDNS Stack →](10-PowerDNS-Stack.md)

---

# 9. Installers & Packaging

Each platform gets a native, double-clickable installer with the same goal: a non-technical user finishes with a running server, an authorized (or declined) Internet link, and a paired local Sunshine. macOS and Linux additionally get a **package-manager path** (Homebrew cask, signed APT/DNF repositories, AUR) — §9.2 and §9.3 — which is the only route that also brings updates. Everything ships from the `release.yml` GitHub Actions workflow (see [Build & CI](11-Build-CI-Testing.md)).

## 9.1 Windows — Inno Setup (`backend/installer/moonlightweb.iss`)

**Artifact**: `MoonlightWeb-installer-<version>-win-{x64,arm64}.exe` (Inno Setup 6, LZMA2, admin privileges, EN/FR/zh-CN — the Chinese translation is vendored as `ChineseSimplified.isl`).

**Wizard flow**:

1. Install the app + Start-Menu shortcuts (app, admin page, uninstaller). The Desktop/Start-Menu shortcuts are **`.lnk` files pointing at the exe** — launching the exe starts the server when down, or surfaces the admin page via the single-instance logic when up.
2. **Internet Link page** — explicit opt-in checkbox (unchecked by default) to publish the public domain.
3. **Sunshine page** — detects an existing Sunshine; offers a silent install (downloads the arch-specific NSIS installer from LizardByte's latest release, runs `/S`); collects Sunshine credentials for auto-pairing (skippable).
4. Drops a **`provisioning.json`** next to the exe; the server consumes it once on first run (`Provisioning::applyOnce`): enable Internet Access (with the consent recorded as `source:"installer"`), create/pair the local Sunshine via its REST API (`/api/pin`).
5. Runs the server during a **live provisioning checklist** page (Sunshine install → pairing → A-record published — the async A-record step is fed back by the server through a status file), then offers to open the admin page (the URL the server actually published, real port/domain).

An optional logon task (`--autostart`) starts the server at login without opening a browser. **No DNS/ACME secrets ship in the installer** — they are compiled into the exe by CI.

**Code signing (SignPath)**: `MoonlightWeb.exe` — the only first-party binary staged (Qt's and OpenSSL's DLLs arrive already signed) — is signed **before** ISCC swallows it, then the installer is signed in turn; each signed file overwrites the unsigned one in place, so the publish steps cannot ship the wrong build by accident. Signing the payload separately is not optional: the service registers the exe to run at boot, and an unsigned copy inside a signed installer is flagged on first launch anyway. ISCC gets no signing arguments (no local signtool on the runner). SignPath's action takes its input from the GitHub artifact store, hence the two `unsigned-*` artifacts with **1-day retention** — intermediates, not deliverables. Without `SIGNPATH_API_TOKEN` / `SIGNPATH_ORGANIZATION_ID` the whole chain is skipped with a `::warning` and the release ships unsigned (SmartScreen will complain), rather than failing.

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

### Homebrew cask — the warning-free path

The `.pkg` is **not notarized**: that needs a paid Apple Developer ID, for which Apple offers no free tier and no open-source programme. A *downloaded* pkg is therefore refused with *"cannot be opened because it is from an unidentified developer"*. That check keys off the **quarantine attribute a browser sets**, not off the file itself — hand the very same `.pkg` to `installer(8)` as root and it installs with no prompt at all. Two paths do exactly that:

- **`brew install --cask linckosz/tap/moonlightweb`** — the cask template is `backend/packaging/homebrew/moonlightweb.rb`; the `homebrew` CI job substitutes the version and the sha256 **of the artifact the `macos` job just produced** (not a re-download: the release asset may not be visible yet) and pushes the result to `linckosz/homebrew-tap` as `Casks/moonlightweb.rb`. It pins `arch: :arm64` and `macos: ">= :monterey"` (matching `LSMinimumSystemVersion` in the bundle), `uninstall` undoes the LaunchAgent + pkgutil receipt, and `zap` removes the user data a plain uninstall keeps. Not submitted to homebrew-cask upstream: that gate is *notability* (~75 stars / 30 forks); a personal tap installs identically with no approval.
- **`curl -fsSL https://moonlightweb.top/install.sh | bash`** — for people without Homebrew. Same asset, same `installer(8)`, and it verifies the download is really a pkg (`xar!` magic bytes) before running it.

Edit the template in this repository, never the copy in the tap — the job overwrites it on every release.

## 9.3 Linux — `.deb` / `.rpm` / AppImage (`backend/packaging/linux/make-packages.sh`)

One **linuxdeploy AppDir** (binary + bundled Qt runtime with `$ORIGIN` rpaths + frontend) is built in CI, then:

- **AppImage** for Arch-based and immutable distros (SteamOS, Bazzite).
- **`.deb` + `.rpm`** via **fpm**: the AppDir tree is relocated under `/opt/moonlightweb`, with a `qt.conf`, a `/usr/bin/moonlightweb` symlink, a hicolor icon and a `.desktop` menu entry (absolute `Exec=`, `/opt` isn't on PATH). **No hard dependencies** — Qt/OpenSSL are bundled; naming distro libs would make the package distro-specific.
- **postinst**: refreshes desktop/icon caches; **opens firewall ports best-effort** (443/tcp, 80/tcp, 47999/udp on firewalld/ufw — Linux firewalls are port-based, and this runs before the app picks a port); **launches the app inside the active graphical session** via `systemd-run --user` (postinst runs as root with no display).
- **prerm**: stops the running instance; removes the firewall rules on uninstall (kept on upgrade).

Autostart uses an XDG autostart entry; a systemd unit (`backend/packaging/systemd/moonlightweb.service`) covers headless/service installs. The Desktop shortcut is a `Type=Application` entry executing the binary (a `Type=Link` renders a generic icon and GNOME refuses to launch it) marked `gio trusted`.

### Software-centre visibility (AppStream)

`backend/packaging/linux/top.moonlightweb.MoonlightWeb.metainfo.xml` is installed to `/usr/share/metainfo/` inside both packages. GNOME Software, KDE Discover and Ubuntu's App Center index **AppStream**, not the package list, and Ubuntu explicitly hides packages that carry none (ubuntu/app-center#1273) — **a `.desktop` file is not enough**. The template holds the long description, categories, keywords, the `<supports>` input kinds, an OARS content rating, and **absolute** screenshot URLs on moonlightweb.top (a software centre fetches them before anything is installed, so a path inside the package would be useless). `make-packages.sh` substitutes `@MW_VERSION@`/`@MW_DATE@`, then validates with `appstreamcli` when it is present — non-fatal, since the runner may not have it. One trap when editing: **an XML comment may not contain `--`**, which rules out writing option flags inside the header comment.

The same file is compiled into the repository index below, so the software-centre listing can never drift from what actually gets installed.

### Signed APT & DNF repositories

`backend/packaging/linux/make-repo.sh <deb> <rpm> <version> <site-dir> <gpg-key-id>` turns the two packages into a static, signed repository tree served at **`https://linckosz.github.io/moonlight-web/`**. That is what upgrades a one-off download into a managed install: `apt install moonlightweb` / `dnf install moonlightweb`, updates through the system's own updater, and an entry in the software centre.

| Piece | How |
|---|---|
| APT index | `apt-ftparchive packages pool` → `Packages{,.gz}` (`gzip -9cn`: no timestamp, so an unchanged index stays byte-identical), then `apt-ftparchive release` → `Release`. Both written to a temp file and copied in — writing via shell redirect into the directory being scanned is not safe. |
| APT signature | `gpg --clearsign` → **`InRelease`** (what modern apt fetches) *and* `--detach-sign --armor` → `Release.gpg` for older clients. Trust flows from the signed Release alone; individual .debs are not signed. |
| DNF index | `createrepo_c` → `repodata/`, plus a detached armored signature on `repomd.xml`. |
| DNF signature | rpm needs **both**: `repo_gpgcheck=1` (the signed `repomd.xml`) *and* `gpgcheck=1` (per-package), so the .rpm itself is `rpmsign --addsign`ed. The CI key has no passphrase and there is no tty → `--pinentry-mode loopback`. |
| DEP-11 | `dpkg-deb -x` the package, then `appstreamcli compose --no-net` over the unpacked tree → `Components-*.yml.gz` + `icons-*.tar.gz` into `dists/stable/main/dep11/`. The output paths have moved between appstreamcli releases, so they are located with `find`; producing none is **fatal** — it is the entire point of the job. |
| Client config | `moonlightweb.gpg` (dearmored, for apt) + `moonlightweb.asc` (armored, for rpm), a deb822 `moonlightweb.sources` pointing at `/etc/apt/keyrings/moonlightweb.gpg`, a `moonlightweb.repo`, `.nojekyll` and a landing `index.html` with the copy-paste recipes. |

**Only the version being released is published.** apt and dnf never resolve anything but the newest, and older packages remain on the releases page, so a pool of past versions would cost bandwidth for nothing. The tree leaves CI as a **GitHub Pages deployment artifact**, never a commit: it is ~100 MB of binaries per release, and a `gh-pages` branch holding them would be fetched by every `git clone` of this repository (clone takes all branches).

Why not a PPA, COPR or OBS: they build **from source on their own infrastructure**, where the DNS/ACME secrets injected at compile time (`target_compile_definitions` in `backend/CMakeLists.txt`) do not exist — the result would silently be a LAN-only build. Signing our own binaries and hosting the index is the only way to ship the same binary the release does. See §9.5 for Flatpak/Snap.

### AUR — `moonlightweb-bin`

Arch and its derivatives are the one family with no repository of ours: there is no stable base to build a shared binary against. `backend/packaging/aur/{PKGBUILD,.SRCINFO}` repackage the released `.deb` instead — `package()` is a single `bsdtar -xf data.tar.*`, with `options=('!strip' '!debug')` since the payload is already built. Both files are rendered from the same version/sha256 substitution: the AUR reads `.SRCINFO` while makepkg reads the `PKGBUILD`, and a mismatch between them is the classic way an AUR package breaks. `.SRCINFO` must stay **tab-indented**.

### `website/install.sh` — one command for macOS and Linux

The dispatcher behind `curl -fsSL https://moonlightweb.top/install.sh | bash`. On macOS it runs the pkg path (§9.2). On Linux it detects the package manager — `apt-get` → keyring + `.sources` + `apt-get install`; `dnf`/`yum`/`zypper` → the `.repo` dropped into `/etc/yum.repos.d` (or `/etc/zypp/repos.d`); `pacman` → `paru`/`yay` on `moonlightweb-bin`, else the AppImage — and falls back to the AppImage (into `~/.local/bin` with a `.desktop` entry) on anything else, which is what immutable systems like SteamOS and Bazzite get. Downloads are checked for their magic bytes (`xar!` for the pkg, `\177ELF` for the AppImage) before anything is executed.

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
| The `.pkg` is unnotarized — a downloaded one is refused by Gatekeeper | Gatekeeper keys off the browser's quarantine attribute, so both the Homebrew cask and `install.sh` hand the same file to `installer(8)` as root and no prompt appears (§9.2). |
| homebrew-cask upstream requires "notability" (~75 stars / 30 forks) | A personal tap (`linckosz/homebrew-tap`) — no approval, identical install. |
| PPA / COPR / OBS build from source on infrastructure that has no access to our secrets | Compile-time DNS/ACME definitions would be absent → a silently LAN-only build. Own signed APT/DNF repositories, shipping the very binary the release ships (§9.3). |
| Flatpak / Snap cannot install Sunshine or open ports | `SunshineInstaller` shells out to `apt-get`/`dnf`/`zypper`/`pacman` and the postinst opens firewalld/ufw — neither is reachable from a sandbox, so neither format is published. |
| A `gh-pages` branch of package binaries would be pulled by every clone | The repository tree is uploaded as a **Pages deployment artifact**; nothing is committed (§9.3). |
| Ubuntu's App Center hides packages with no AppStream metadata | A `metainfo.xml` inside the package + a DEP-11 index in the repository (§9.3) — a `.desktop` entry alone is invisible. |
| Windows service session 0 has no desktop | `MW_SERVICE` suppresses tray/shortcut/browser behaviors. |
| A remote browser cannot answer a UAC / polkit / `osascript` prompt on the host desktop | Windows: elevated trigger-less scheduled task started with `schtasks /Run` (§9.1). Linux AppImage: in-place file replacement, no root at all. Service/root installs: already elevated. Everything else reports `requires_host_confirmation` so the UI says "confirmation required on the host PC" instead of hanging on an invisible dialog. |
| An elevated task running an exe from a writable directory is a privilege-escalation hole | Staging lives under per-user `%LocalAppData%` (expanded by Task Scheduler in the task's own user context), never `%ProgramData%`. |
| Inno Setup can't know the final admin URL | Server publishes `admin_url` via `Provisioning::setInfo`; installer reads it post-install with a provisional fallback. |

---

[← REST API](08-REST-API.md) · [Home](Home.md) · [Next: PowerDNS Stack →](10-PowerDNS-Stack.md)
