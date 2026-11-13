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

#include "LauncherDetect.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

#include <algorithm>

namespace LauncherDetect {

namespace {

const QString kSteamOpen = QStringLiteral("steam://open/bigpicture");
const QString kSteamClose = QStringLiteral("steam://close/bigpicture");

Launcher steamBigPicture(const QString& iconPath)
{
    return Launcher{QStringLiteral("steam"), QStringLiteral("Steam Big Picture"), kSteamOpen,
                    kSteamClose, iconPath};
}

/// A Windows path, written with backslashes whatever the OS this runs on: the
/// lookups describe a Windows machine, and Linux and macOS test them too, where
/// QDir::toNativeSeparators would leave Steam's forward slashes as they are.
/// A share's leading "\\" is kept: cleanPath keeps it only on Windows.
QString windowsPath(const QString& path)
{
    QString p = path.trimmed();
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const bool share = p.startsWith(QLatin1String("//"));
    p = QDir::cleanPath(p);
    if (share && !p.startsWith(QLatin1String("//"))) p.prepend(QLatin1Char('/'));
    return p.replace(QLatin1Char('/'), QLatin1Char('\\'));
}

/// Joins @p dir and @p file with a backslash, the way Windows writes it.
QString windowsJoin(const QString& dir, const QString& file)
{
    QString base = dir.trimmed();
    while (base.endsWith(QLatin1Char('\\')) || base.endsWith(QLatin1Char('/')))
        base.chop(1);
    return windowsPath(base + QLatin1Char('\\') + file);
}

QString windowsDir(const QString& program)
{
    const int cut =
        (std::max)(program.lastIndexOf(QLatin1Char('\\')), program.lastIndexOf(QLatin1Char('/')));
    return cut > 0 ? program.left(cut) : QString();
}

} // namespace

QString programOf(const QString& commandLine)
{
    const QString line = commandLine.trimmed();
    if (line.startsWith(QLatin1Char('"'))) {
        const int close = line.indexOf(QLatin1Char('"'), 1);
        return close > 1 ? line.mid(1, close - 1) : QString();
    }
    // Unquoted: up to the .exe, which may sit in a path with spaces.
    const int exe = line.indexOf(QStringLiteral(".exe"), 0, Qt::CaseInsensitive);
    if (exe > 0) return line.left(exe + 4);
    return line.section(QLatin1Char(' '), 0, 0);
}

std::optional<Launcher> steamWindows(const Lookup& lookup)
{
    QStringList candidates;
    // Steam writes it with forward slashes, in lower case.
    const QString userExe =
        lookup.registry(QStringLiteral("HKCU\\Software\\Valve\\Steam"), QStringLiteral("SteamExe"));
    if (!userExe.isEmpty()) candidates << windowsPath(userExe);
    const QString handler =
        lookup.registry(QStringLiteral("HKCR\\steam\\Shell\\Open\\Command"), QString());
    if (!handler.isEmpty()) candidates << windowsPath(programOf(handler));
    for (const QString& key : {QStringLiteral("HKLM\\SOFTWARE\\WOW6432Node\\Valve\\Steam"),
                               QStringLiteral("HKLM\\SOFTWARE\\Valve\\Steam")}) {
        const QString dir = lookup.registry(key, QStringLiteral("InstallPath"));
        if (!dir.isEmpty()) candidates << windowsJoin(dir, QStringLiteral("steam.exe"));
    }
    for (const QString& exe : candidates)
        if (exe.endsWith(QStringLiteral("steam.exe"), Qt::CaseInsensitive) && lookup.exists(exe))
            return steamBigPicture(exe);
    return std::nullopt;
}

std::optional<Launcher> playniteWindows(const Lookup& lookup)
{
    QStringList dirs;
    for (const QString& root :
         {QStringLiteral("HKCU\\Software\\Classes"), QStringLiteral("HKCR")}) {
        const QString handler =
            lookup.registry(root + QStringLiteral("\\playnite\\shell\\open\\command"), QString());
        if (!handler.isEmpty()) dirs << windowsDir(windowsPath(programOf(handler)));
    }
    for (const QString& uninstall :
         {QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
          QStringLiteral("HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
          QStringLiteral(
              "HKLM\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall")}) {
        for (const QString& sub : lookup.subkeys(uninstall)) {
            const QString key = uninstall + QLatin1Char('\\') + sub;
            if (!lookup.registry(key, QStringLiteral("DisplayName"))
                     .startsWith(QStringLiteral("Playnite"), Qt::CaseInsensitive))
                continue;
            const QString location = lookup.registry(key, QStringLiteral("InstallLocation"));
            if (!location.isEmpty()) dirs << location;
        }
    }
    const QString local = lookup.env(QStringLiteral("LOCALAPPDATA"));
    if (!local.isEmpty()) dirs << windowsJoin(local, QStringLiteral("Playnite"));
    for (const QString& dir : dirs) {
        if (dir.isEmpty()) continue;
        const QString exe = windowsJoin(dir, QStringLiteral("Playnite.FullscreenApp.exe"));
        if (lookup.exists(exe))
            return Launcher{QStringLiteral("playnite"), QStringLiteral("Playnite"),
                            QLatin1Char('"') + exe + QLatin1Char('"'), QString(), exe};
    }
    return std::nullopt;
}

std::optional<Launcher> steamMac(const Lookup& lookup)
{
    QStringList bundles{QStringLiteral("/Applications/Steam.app")};
    const QString home = lookup.env(QStringLiteral("HOME"));
    if (!home.isEmpty()) bundles << home + QStringLiteral("/Applications/Steam.app");
    for (const QString& bundle : bundles)
        if (lookup.exists(bundle)) return steamBigPicture(bundle);
    return std::nullopt;
}

std::optional<Launcher> steamLinux(const Lookup& lookup)
{
    const QString home = lookup.env(QStringLiteral("HOME"));
    QStringList installs{QStringLiteral("/usr/bin/steam"), QStringLiteral("/usr/games/steam"),
                         QStringLiteral("/snap/bin/steam"),
                         QStringLiteral("/var/lib/flatpak/app/com.valvesoftware.Steam")};
    if (!home.isEmpty())
        installs << home + QStringLiteral("/.local/share/flatpak/app/com.valvesoftware.Steam");
    bool found = false;
    for (const QString& path : installs)
        if (lookup.exists(path)) found = true;
    if (!found) return std::nullopt;

    // Its icon, looked up by hand in the icon theme's directories: a server
    // with no desktop session has no theme to ask.
    QStringList shares;
    const QString dataHome = lookup.env(QStringLiteral("XDG_DATA_HOME"));
    if (!dataHome.isEmpty())
        shares << dataHome;
    else if (!home.isEmpty())
        shares << home + QStringLiteral("/.local/share");
    QString dataDirs = lookup.env(QStringLiteral("XDG_DATA_DIRS"));
    if (dataDirs.isEmpty()) dataDirs = QStringLiteral("/usr/local/share:/usr/share");
    shares << dataDirs.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    shares << QStringLiteral("/var/lib/flatpak/exports/share");
    if (!home.isEmpty()) shares << home + QStringLiteral("/.local/share/flatpak/exports/share");
    QString icon;
    for (const QString& size :
         {QStringLiteral("256x256"), QStringLiteral("128x128"), QStringLiteral("96x96"),
          QStringLiteral("64x64"), QStringLiteral("48x48")}) {
        for (const QString& share : shares) {
            for (const QString& name :
                 {QStringLiteral("steam"), QStringLiteral("com.valvesoftware.Steam")}) {
                const QString path = share + QStringLiteral("/icons/hicolor/") + size +
                                     QStringLiteral("/apps/") + name + QStringLiteral(".png");
                if (icon.isEmpty() && lookup.exists(path)) icon = path;
            }
        }
    }
    if (icon.isEmpty() && lookup.exists(QStringLiteral("/usr/share/pixmaps/steam.png")))
        icon = QStringLiteral("/usr/share/pixmaps/steam.png");
    return steamBigPicture(icon);
}

QVector<Launcher> detect(const Lookup& lookup)
{
    QVector<Launcher> out;
#if defined(Q_OS_WIN)
    if (auto steam = steamWindows(lookup)) out.append(*steam);
    if (auto playnite = playniteWindows(lookup)) out.append(*playnite);
#elif defined(Q_OS_MACOS)
    if (auto steam = steamMac(lookup)) out.append(*steam);
#elif defined(Q_OS_LINUX)
    if (auto steam = steamLinux(lookup)) out.append(*steam);
#else
    Q_UNUSED(lookup);
#endif
    return out;
}

QVector<Launcher> detect()
{
    return detect(systemLookup());
}

Lookup systemLookup()
{
    Lookup lookup;
    lookup.exists = [](const QString& path) { return QFileInfo::exists(path); };
    lookup.env = [](const QString& name) {
        return qEnvironmentVariable(name.toUtf8().constData());
    };
#if defined(Q_OS_WIN)
    // QSettings names the hives in full; the rules write them short.
    const auto native = [](const QString& key) {
        QString path = key;
        if (path.startsWith(QStringLiteral("HKCU\\")))
            path.replace(0, 4, QStringLiteral("HKEY_CURRENT_USER"));
        else if (path.startsWith(QStringLiteral("HKLM\\")))
            path.replace(0, 4, QStringLiteral("HKEY_LOCAL_MACHINE"));
        else if (path.startsWith(QStringLiteral("HKCR\\")))
            path.replace(0, 4, QStringLiteral("HKEY_CLASSES_ROOT"));
        return path;
    };
    lookup.registry = [native](const QString& key, const QString& name) {
        QSettings reg(native(key), QSettings::Registry64Format);
        // "." is the key's own (default) value.
        return reg.value(name.isEmpty() ? QStringLiteral(".") : name).toString();
    };
    lookup.subkeys = [native](const QString& key) {
        return QSettings(native(key), QSettings::Registry64Format).childGroups();
    };
#else
    lookup.registry = [](const QString&, const QString&) { return QString(); };
    lookup.subkeys = [](const QString&) { return QStringList(); };
#endif
    return lookup;
}

QJsonObject toJson(const Launcher& launcher)
{
    QJsonObject out{
        {"source", launcher.source}, {"name", launcher.name}, {"command", launcher.command}};
    if (!launcher.quitCommand.isEmpty()) out["quit_command"] = launcher.quitCommand;
    return out;
}

QJsonArray toJson(const QVector<Launcher>& launchers)
{
    QJsonArray out;
    for (const Launcher& launcher : launchers)
        out.append(toJson(launcher));
    return out;
}

} // namespace LauncherDetect
