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

#include "Autostart.h"

#include "common/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace Autostart {

#if defined(Q_OS_MACOS)

namespace {

const QString kLabel = QStringLiteral("com.moonlightweb.agent");

QString plistPath()
{
    return QDir::homePath() + QStringLiteral("/Library/LaunchAgents/") + kLabel +
           QStringLiteral(".plist");
}

// Minimal XML text escaping for values embedded in the plist (paths may contain
// '&' etc.). Order matters: '&' first.
QString xmlEscape(QString s)
{
    s.replace('&', QStringLiteral("&amp;"));
    s.replace('<', QStringLiteral("&lt;"));
    s.replace('>', QStringLiteral("&gt;"));
    return s;
}

} // namespace

bool installLoginItem()
{
    const QString exe = QCoreApplication::applicationFilePath();
    const QString logDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/logs");
    QDir().mkpath(logDir);
    const QString log = logDir + QStringLiteral("/agent.log");

    // No MW_SERVICE here: this is a normal GUI launch (tray icon), mirroring the
    // Windows logon Scheduled Task. KeepAlive/SuccessfulExit=false → relaunch only
    // on a crash, never after a clean quit.
    const QString plist = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                         "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                                         "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                                         "<plist version=\"1.0\">\n"
                                         "<dict>\n"
                                         "    <key>Label</key><string>%1</string>\n"
                                         "    <key>ProgramArguments</key>\n"
                                         "    <array><string>%2</string></array>\n"
                                         "    <key>RunAtLoad</key><true/>\n"
                                         "    <key>KeepAlive</key>\n"
                                         "    <dict><key>SuccessfulExit</key><false/></dict>\n"
                                         "    <key>ThrottleInterval</key><integer>5</integer>\n"
                                         "    <key>StandardOutPath</key><string>%3</string>\n"
                                         "    <key>StandardErrorPath</key><string>%3</string>\n"
                                         "</dict>\n"
                                         "</plist>\n")
                              .arg(xmlEscape(kLabel), xmlEscape(exe), xmlEscape(log));

    const QString path = plistPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        Logger::warning(QStringLiteral("Autostart: cannot write %1").arg(path));
        return false;
    }
    f.write(plist.toUtf8());
    f.close();
    // Do NOT `launchctl bootstrap` here: this instance is already running, and
    // loading the agent now would launch a duplicate. launchd loads the plist at
    // the next login on its own.
    Logger::info(QStringLiteral("Autostart: login item installed at %1").arg(path));
    return true;
}

bool isLoginItemInstalled()
{
    return QFile::exists(plistPath());
}

#else // non-macOS

bool installLoginItem()
{
    return false;
}

bool isLoginItemInstalled()
{
    return false;
}

#endif

} // namespace Autostart
