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

#pragma once

#include <QDir>
#include <QString>
#include <QtGlobal>

namespace mw::win {

/// Whether `path` lies under %SystemRoot%\System32.
///
/// A LocalSystem service's profile lives there (config\systemprofile), so its
/// LOCALAPPDATA and Qt's AppDataLocation do too. The installers such a service
/// runs are 32-bit — Inno Setup's loader, the ViGEmBus WiX bundle — and WOW64
/// quietly redirects a 32-bit process's System32 to SysWOW64. Started from a
/// System32 path, the installer cannot open its own file: it exits with code 1
/// ("The system cannot find the path specified", swallowed by
/// /SUPPRESSMSGBOXES) before writing one log line. Nothing handed to one of
/// those installers — the executable, its /LOG — may live under this directory.
inline bool isUnderSystem32(const QString& path)
{
    // Backslashes replaced by hand, not with QDir::fromNativeSeparators: that is
    // a no-op off Windows, and the TNR checks this on every platform.
    const auto normalized = [](QString p) {
        return QDir::cleanPath(p.replace(QLatin1Char('\\'), QLatin1Char('/'))) + QLatin1Char('/');
    };
    QString root = qEnvironmentVariable("SystemRoot");
    if (root.isEmpty()) root = QStringLiteral("C:/Windows");
    const QString system32 = normalized(root + QStringLiteral("/System32"));
    const QString candidate = normalized(path);
    return candidate.startsWith(system32, Qt::CaseInsensitive);
}

} // namespace mw::win
