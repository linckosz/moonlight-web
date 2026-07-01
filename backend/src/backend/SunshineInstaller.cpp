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

QString installMacOS(const QString& user, const QString& pass)
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
    if (run(QStringLiteral("/usr/bin/hdiutil"),
            {"attach", dmg, "-nobrowse", "-noverify", "-mountpoint", mnt}, 60000, &out) != 0)
        return QStringLiteral("Could not mount the disk image: %1").arg(out.trimmed());

    QString copyErr;
    const QString src = mnt + QStringLiteral("/Sunshine.app");
    const QString dst = QStringLiteral("/Applications/Sunshine.app");
    if (!QFileInfo::exists(src)) {
        copyErr = QStringLiteral("Sunshine.app not found in the disk image");
    } else {
        // Replace any previous copy so an upgrade doesn't merge stale files.
        run(QStringLiteral("/bin/rm"), {"-rf", dst}, 30000);
        if (run(QStringLiteral("/bin/cp"), {"-R", src, QStringLiteral("/Applications/")}, 120000,
                &out) != 0)
            copyErr = QStringLiteral("Copy to /Applications failed: %1").arg(out.trimmed());
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

} // namespace SunshineInstaller
