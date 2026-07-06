/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "SunshineInstaller.h"

#include "../common/Logger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>

namespace SunshineInstaller {

namespace {

// Run a command synchronously; returns the exit code (-1 on failure to start /
// timeout). Captured stdout+stderr is appended to `out` when provided.
int run(const QString& program, const QStringList& args, int timeoutMs = 120000,
        QString* out = nullptr)
{
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(program, args);
    if (!proc.waitForStarted(10000)) return -1;
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(2000);
        return -1;
    }
    if (out) *out = QString::fromUtf8(proc.readAll());
    return proc.exitCode();
}

// Resolve a binary on PATH via the platform's lookup tool.
QString which(const QString& name)
{
    QString out;
#ifdef Q_OS_WIN
    if (run(QStringLiteral("where"), {name}, 5000, &out) == 0) {
#else
    if (run(QStringLiteral("/usr/bin/which"), {name}, 5000, &out) == 0) {
#endif
        const QString first = out.split('\n', Qt::SkipEmptyParts).value(0).trimmed();
        if (!first.isEmpty() && QFileInfo::exists(first)) return first;
    }
    return QString();
}

} // namespace

DetectResult detect()
{
    DetectResult r;

#if defined(Q_OS_MACOS)
    for (const QString& p : {QStringLiteral("/Applications/Sunshine.app/Contents/MacOS/sunshine"),
                             QStringLiteral("/opt/homebrew/bin/sunshine"),
                             QStringLiteral("/usr/local/bin/sunshine")}) {
        if (QFileInfo::exists(p)) {
            r.installed = true;
            r.exePath = p;
            return r;
        }
    }
#elif defined(Q_OS_WIN)
    const QString pf = qEnvironmentVariable("ProgramFiles", QStringLiteral("C:/Program Files"));
    const QString p = pf + QStringLiteral("/Sunshine/sunshine.exe");
    if (QFileInfo::exists(p)) {
        r.installed = true;
        r.exePath = p;
        return r;
    }
#else
    for (const QString& p :
         {QStringLiteral("/usr/bin/sunshine"), QStringLiteral("/usr/local/bin/sunshine")}) {
        if (QFileInfo::exists(p)) {
            r.installed = true;
            r.exePath = p;
            return r;
        }
    }
#endif

    const QString onPath = which(QStringLiteral("sunshine"));
    if (!onPath.isEmpty()) {
        r.installed = true;
        r.exePath = onPath;
    }
    return r;
}

#if defined(Q_OS_LINUX)
namespace {

// Package family this distro belongs to, mapped to the matching official
// Sunshine release asset + the package manager that installs a local file.
enum class PkgFamily
{
    None,
    Apt,
    Dnf,
    Zypper,
    Pacman
};

struct DistroInfo
{
    PkgFamily family = PkgFamily::None;
    QString id;        // ID= from /etc/os-release
    QString idLike;    // ID_LIKE=
    QString versionId; // VERSION_ID=
    QString debArch;   // amd64 | arm64
    QString rpmArch;   // x86_64 | aarch64
};

DistroInfo detectDistro()
{
    DistroInfo d;
    const QString cpu = QSysInfo::currentCpuArchitecture();
    if (cpu == QLatin1String("x86_64")) {
        d.debArch = QStringLiteral("amd64");
        d.rpmArch = QStringLiteral("x86_64");
    } else if (cpu == QLatin1String("arm64") || cpu == QLatin1String("aarch64")) {
        d.debArch = QStringLiteral("arm64");
        d.rpmArch = QStringLiteral("aarch64");
    } else {
        return d;
    }

    QFile f(QStringLiteral("/etc/os-release"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return d;
    const QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
    for (const QString& line : lines) {
        const int eq = line.indexOf('=');
        if (eq <= 0) continue;
        const QString key = line.left(eq).trimmed();
        QString val = line.mid(eq + 1).trimmed();
        if (val.size() >= 2 && val.startsWith('"') && val.endsWith('"'))
            val = val.mid(1, val.size() - 2);
        if (key == QLatin1String("ID"))
            d.id = val;
        else if (key == QLatin1String("ID_LIKE"))
            d.idLike = val;
        else if (key == QLatin1String("VERSION_ID"))
            d.versionId = val;
    }

    const auto like = [&d](const char* n) { return d.idLike.contains(QLatin1String(n)); };
    if (d.id == QLatin1String("ubuntu") || d.id == QLatin1String("debian") || like("ubuntu") ||
        like("debian"))
        d.family = PkgFamily::Apt;
    else if (d.id == QLatin1String("fedora") || like("fedora"))
        d.family = PkgFamily::Dnf;
    else if (d.id.startsWith(QLatin1String("opensuse")) || like("suse"))
        d.family = PkgFamily::Zypper;
    else if (d.id == QLatin1String("arch") || like("arch"))
        d.family = PkgFamily::Pacman;
    return d;
}

// Path of the family's package manager, empty when absent.
QString familyTool(PkgFamily fam)
{
    switch (fam) {
    case PkgFamily::Apt: return which(QStringLiteral("apt-get"));
    case PkgFamily::Dnf: return which(QStringLiteral("dnf"));
    case PkgFamily::Zypper: return which(QStringLiteral("zypper"));
    case PkgFamily::Pacman: return which(QStringLiteral("pacman"));
    default: return QString();
    }
}

// Debian-family assets have stable names, addressable via latest/download.
QString debAssetFor(const DistroInfo& d)
{
    if (d.id == QLatin1String("ubuntu")) {
        // Pick the newest asset series not newer than this Ubuntu release
        // (assets track the LTS series + the upcoming one).
        const double v = d.versionId.toDouble(); // "24.04" → 24.04
        QString series = QStringLiteral("22.04");
        if (v >= 26.04)
            series = QStringLiteral("26.04");
        else if (v >= 24.04)
            series = QStringLiteral("24.04");
        return QStringLiteral("sunshine-ubuntu-%1-%2.deb").arg(series, d.debArch);
    }
    // Derivatives (Mint, Pop!_OS…) version differently; assume the current LTS.
    if (d.idLike.contains(QLatin1String("ubuntu")))
        return QStringLiteral("sunshine-ubuntu-24.04-%1.deb").arg(d.debArch);
    return QStringLiteral("sunshine-debian-trixie-%1.deb").arg(d.debArch);
}

// Download a URL to a file (curl preferred; Ubuntu desktop ships wget only).
// Returns an empty string on success, else a human-readable error.
QString fetchToFile(const QString& url, const QString& dest)
{
    QString out;
    const QString curl = which(QStringLiteral("curl"));
    if (!curl.isEmpty()) {
        if (run(curl, {"-fLsS", "-o", dest, url}, 300000, &out) == 0) return QString();
        return QStringLiteral("Download failed: %1").arg(out.trimmed());
    }
    const QString wget = which(QStringLiteral("wget"));
    if (wget.isEmpty())
        return QStringLiteral("Neither curl nor wget is available to download Sunshine");
    if (run(wget, {"-qO", dest, url}, 300000, &out) == 0) return QString();
    return QStringLiteral("Download failed: %1").arg(out.trimmed());
}

// rpm/pacman asset names embed the release tag, so they cannot be addressed via
// latest/download: list the latest release's download URLs from the GitHub API
// and keep those whose filename matches `namePattern`.
QStringList resolveAssetUrls(const QRegularExpression& namePattern, QString* err)
{
    const QString api =
        QStringLiteral("https://api.github.com/repos/LizardByte/Sunshine/releases/latest");
    QString json;
    QString out;
    const QString curl = which(QStringLiteral("curl"));
    if (!curl.isEmpty() && run(curl, {"-fLsS", api}, 60000, &out) == 0) {
        json = out;
    } else {
        const QString wget = which(QStringLiteral("wget"));
        if (wget.isEmpty() || run(wget, {"-qO-", api}, 60000, &out) != 0) {
            if (err) *err = QStringLiteral("Could not query the Sunshine release list");
            return {};
        }
        json = out;
    }

    static const QRegularExpression urlRe(
        QStringLiteral("\"browser_download_url\"\\s*:\\s*\"([^\"]+)\""));
    QStringList matches;
    auto it = urlRe.globalMatch(json);
    while (it.hasNext()) {
        const QString url = it.next().captured(1);
        const QString name = url.section('/', -1);
        if (namePattern.match(name).hasMatch()) matches << url;
    }
    if (matches.isEmpty() && err)
        *err = QStringLiteral("No Sunshine package found for this distribution");
    return matches;
}

// Download + install the official Sunshine package for this distro family,
// then apply the credentials.
QString installLinux(const QString& user, const QString& pass)
{
    const DistroInfo d = detectDistro();
    if (d.family == PkgFamily::None)
        return QStringLiteral(
            "No prebuilt Sunshine package for this distribution — install it manually");
    const QString tool = familyTool(d.family);
    if (tool.isEmpty()) return QStringLiteral("Package manager not found for this distribution");

    QString url;
    QString file;
    QStringList installArgs; // argv run under pkexec (root)
    QString err;

    switch (d.family) {
    case PkgFamily::Apt:
        url = QStringLiteral("https://github.com/LizardByte/Sunshine/releases/latest/download/%1")
                  .arg(debAssetFor(d));
        file = QDir::tempPath() + QStringLiteral("/mw-sunshine.deb");
        // apt-get resolves the .deb's dependencies (unlike dpkg -i).
        installArgs = {tool, QStringLiteral("install"), QStringLiteral("-y"), file};
        break;
    case PkgFamily::Dnf: {
        // Assets are per Fedora release: Sunshine-<tag>-1.fcNN.<arch>.rpm — take
        // the newest fcNN not newer than this system.
        const QRegularExpression re(QStringLiteral("\\.fc(\\d+)\\.%1\\.rpm$").arg(d.rpmArch));
        const QStringList urls = resolveAssetUrls(re, &err);
        const int rel = d.versionId.toInt();
        int best = -1;
        for (const QString& u : urls) {
            const int fc = re.match(u.section('/', -1)).captured(1).toInt();
            if (fc <= rel && fc > best) {
                best = fc;
                url = u;
            }
        }
        if (url.isEmpty())
            return err.isEmpty() ? QStringLiteral("No Sunshine package for this Fedora release — "
                                                  "install it manually")
                                 : err;
        file = QDir::tempPath() + QStringLiteral("/mw-sunshine.rpm");
        installArgs = {tool, QStringLiteral("install"), QStringLiteral("-y"), file};
        break;
    }
    case PkgFamily::Zypper: {
        const QRegularExpression re(QStringLiteral("\\.suse\\..*%1\\.rpm$").arg(d.rpmArch));
        const QStringList urls = resolveAssetUrls(re, &err);
        if (urls.isEmpty()) return err;
        url = urls.first();
        file = QDir::tempPath() + QStringLiteral("/mw-sunshine.rpm");
        installArgs = {tool, QStringLiteral("--non-interactive"), QStringLiteral("--no-gpg-checks"),
                       QStringLiteral("install"), file};
        break;
    }
    case PkgFamily::Pacman: {
        const QRegularExpression re(QStringLiteral("-%1\\.pkg\\.tar\\.zst$").arg(d.rpmArch));
        const QStringList urls = resolveAssetUrls(re, &err);
        if (urls.isEmpty()) return err;
        url = urls.first();
        file = QDir::tempPath() + QStringLiteral("/mw-sunshine.pkg.tar.zst");
        installArgs = {tool, QStringLiteral("-U"), QStringLiteral("--noconfirm"), file};
        break;
    }
    default: return QStringLiteral("Unsupported distribution");
    }

    Logger::info(QStringLiteral("SunshineInstaller: downloading %1").arg(url));
    err = fetchToFile(url, file);
    if (!err.isEmpty()) return err;

    // Install as root via polkit — pkexec pops the password dialog in the GUI
    // session.
    QString out;
    const int rc = run(QStringLiteral("/usr/bin/pkexec"), installArgs, 600000, &out);
    QFile::remove(file);
    if (rc != 0) return QStringLiteral("Package installation failed: %1").arg(out.trimmed());

    if (!setCredentials(user, pass))
        Logger::warning(QStringLiteral("SunshineInstaller: setting credentials failed"));

    Logger::info(QStringLiteral("SunshineInstaller: Sunshine installed via %1").arg(tool));
    return QString();
}

} // namespace
#endif // Q_OS_LINUX

bool canAutoInstall()
{
#if defined(Q_OS_MACOS)
    return true;
#elif defined(Q_OS_LINUX)
    // No network here (called by every /api/setup/status poll): family + tools
    // presence only; the exact release asset is resolved at install time.
    const DistroInfo d = detectDistro();
    return d.family != PkgFamily::None && QFileInfo::exists(QStringLiteral("/usr/bin/pkexec")) &&
           !familyTool(d.family).isEmpty();
#else
    return false;
#endif
}

#if !defined(Q_OS_LINUX) // Linux dispatches to installLinux; avoid an unused static.
static QString installMacOS(const QString& user, const QString& pass)
{
#ifndef Q_OS_MACOS
    Q_UNUSED(user);
    Q_UNUSED(pass);
    return QStringLiteral("Automatic Sunshine install is only supported on macOS");
#else
    // Asset names follow the CPU arch: Sunshine-macOS-{arm64,x86_64}.dmg.
    const QString arch = QSysInfo::currentCpuArchitecture(); // "arm64" | "x86_64"
    const QString url =
        QStringLiteral("https://github.com/LizardByte/Sunshine/releases/latest/download/"
                       "Sunshine-macOS-%1.dmg")
            .arg(arch);
    const QString dmg = QDir::tempPath() + QStringLiteral("/mw-sunshine.dmg");
    const QString mnt = QDir::tempPath() + QStringLiteral("/mw-sunshine-mnt");

    Logger::info(QStringLiteral("SunshineInstaller: downloading %1").arg(url));
    QString out;
    // -L follow redirects, --fail non-2xx → non-zero exit, -o output path.
    if (run(QStringLiteral("/usr/bin/curl"), {"-fLsS", "-o", dmg, url}, 300000, &out) != 0)
        return QStringLiteral("Download failed: %1").arg(out.trimmed());

    QDir().mkpath(mnt);
    // The Sunshine DMG embeds a GPL software-license agreement: `hdiutil attach`
    // prints it and cancels the mount when it has no TTY to accept it (our
    // QProcess has none). Pipe `yes` in via a shell to auto-accept. Single-quote
    // the paths (they live under QDir::tempPath(), but be safe).
    const auto shq = [](const QString& s) {
        return QStringLiteral("'") + QString(s).replace('\'', QStringLiteral("'\\''")) +
               QStringLiteral("'");
    };
    const QString attachCmd =
        QStringLiteral(
            "/usr/bin/yes | /usr/bin/hdiutil attach %1 -nobrowse -noverify -mountpoint %2")
            .arg(shq(dmg), shq(mnt));
    if (run(QStringLiteral("/bin/sh"), {"-c", attachCmd}, 60000, &out) != 0)
        return QStringLiteral("Could not mount the disk image: %1").arg(out.trimmed());

    QString copyErr;
    const QString src = mnt + QStringLiteral("/Sunshine.app");
    const QString dst = QStringLiteral("/Applications/Sunshine.app");
    if (!QFileInfo::exists(src)) {
        copyErr = QStringLiteral("Sunshine.app not found in the disk image");
    } else {
        // Replace any previous copy so an upgrade doesn't merge stale files.
        // Try an unprivileged copy first: admin users can write /Applications,
        // so the common case installs silently in the background. Only when that
        // fails (standard user, or a locked-down /Applications) fall back to a
        // copy run as root through an Authorization Services prompt — osascript
        // "with administrator privileges" is the macOS counterpart to Linux's
        // pkexec, popping the native admin-password dialog in the GUI session.
        run(QStringLiteral("/bin/rm"), {"-rf", dst}, 30000);
        if (run(QStringLiteral("/bin/cp"), {"-R", src, QStringLiteral("/Applications/")}, 120000,
                &out) != 0) {
            const QString script =
                QStringLiteral("do shell script \"/bin/rm -rf '/Applications/Sunshine.app' && "
                               "/bin/cp -R '%1' '/Applications/' && /usr/bin/xattr -dr "
                               "com.apple.quarantine '/Applications/Sunshine.app'\" "
                               "with administrator privileges")
                    .arg(src);
            // Blocks on the password dialog: allow the user time to authenticate.
            if (run(QStringLiteral("/usr/bin/osascript"), {"-e", script}, 180000, &out) != 0)
                copyErr = QStringLiteral("Copy to /Applications failed: %1").arg(out.trimmed());
        }
    }

    // Always detach the image, even on copy failure.
    run(QStringLiteral("/usr/bin/hdiutil"), {"detach", mnt, "-force"}, 60000);
    QFile::remove(dmg);
    if (!copyErr.isEmpty()) return copyErr;

    // Strip the download quarantine so Sunshine launches without a Gatekeeper
    // block (best-effort — a signed .app may not carry the attribute).
    run(QStringLiteral("/usr/bin/xattr"), {"-dr", "com.apple.quarantine", dst}, 30000);

    if (!setCredentials(user, pass))
        Logger::warning(QStringLiteral("SunshineInstaller: setting credentials failed"));

    Logger::info(QStringLiteral("SunshineInstaller: Sunshine installed to %1").arg(dst));
    return QString();
#endif
}
#endif // !Q_OS_LINUX

QString install(const QString& user, const QString& pass)
{
#if defined(Q_OS_LINUX)
    return installLinux(user, pass);
#else
    return installMacOS(user, pass);
#endif
}

bool setCredentials(const QString& user, const QString& pass)
{
    if (user.isEmpty() || pass.isEmpty()) return false;
    DetectResult r = detect();
    if (!r.installed) return false;
    // `sunshine --creds <user> <pass>` sets the web-UI Basic-Auth credentials and
    // exits without starting the server.
    return run(r.exePath, {"--creds", user, pass}, 30000) == 0;
}

bool launch()
{
    DetectResult r = detect();
    if (!r.installed) return false;
#if defined(Q_OS_MACOS)
    // `open -a` starts the bundle in the user's GUI session so its permission
    // prompts (Screen Recording / Accessibility) are shown.
    return run(QStringLiteral("/usr/bin/open"), {"-a", QStringLiteral("Sunshine")}, 15000) == 0;
#else
    return QProcess::startDetached(r.exePath, {});
#endif
}

bool isRunning()
{
#if defined(Q_OS_WIN)
    // tasklist filtered on the image name; the header line only prints when a
    // match exists, so a mention of the exe in the output means it's running.
    QString out;
    if (run(QStringLiteral("tasklist"), {"/FI", "IMAGENAME eq sunshine.exe", "/NH"}, 10000, &out) !=
        0)
        return false;
    return out.contains(QStringLiteral("sunshine.exe"), Qt::CaseInsensitive);
#else
    // pgrep -x matches the exact process name; exit 0 = at least one match.
    return run(QStringLiteral("/usr/bin/pgrep"), {"-x", "sunshine"}, 10000) == 0;
#endif
}

bool stop()
{
#if defined(Q_OS_WIN)
    // On Windows Sunshine runs as the LocalSystem "SunshineService", which
    // supervises and respawns sunshine.exe. A non-elevated taskkill is denied,
    // and stopping the service needs admin (which MoonlightWeb's logon task does
    // not have). We don't try — Windows users stop it from Sunshine's tray icon
    // or Services. The admin UI hides the Stop button on Windows accordingly.
    Logger::info(QStringLiteral(
        "SunshineInstaller: stop() is a no-op on Windows (SunshineService is admin-managed)"));
    return false;
#else
    // pkill's default signal is SIGTERM, so Sunshine can shut down cleanly
    // (deregister its mDNS service, remove the menu-bar/tray item). `-x` matches
    // the exact process name ("sunshine"). Exit 0 = at least one was signaled.
    return run(QStringLiteral("/usr/bin/pkill"), {"-x", "sunshine"}, 15000) == 0;
#endif
}

} // namespace SunshineInstaller
