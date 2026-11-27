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

#include "common/Logger.h"

#include <QString>
#include <QStringList>

namespace mw {

/// Whether @p url carries a key in its fragment: "#k=…", or "k=" among the
/// fragment's "&"-separated items.
inline bool linkCarriesKey(const QString& url)
{
    const qsizetype hash = url.indexOf(QLatin1Char('#'));
    if (hash < 0) return false;
    const QStringList items = url.mid(hash + 1).split(QLatin1Char('&'));
    for (const QString& item : items)
        if (item.startsWith(QLatin1String("k="))) return true;
    return false;
}

/// Whether @p url may be given to a browser this process starts (OpenInBrowser.h).
///
/// Not a link that carries a key: a browser is started on a command line, and
/// a key reaches its page by the page's own request on this machine's loopback
/// (server/OwnerHandoff.h). Refused with one warning, which does not repeat the
/// link. macOS starts a browser through LaunchServices, with no command line
/// to put the link on, and gives it the owner's link as it is.
inline bool mayGiveBrowser(const QString& url)
{
#if defined(Q_OS_MACOS)
    Q_UNUSED(url);
    return true;
#else
    if (!linkCarriesKey(url)) return true;
    Logger::warning(QStringLiteral("[Browser] A link that carries a key was not opened: a browser "
                                   "is given this machine's handoff page instead"));
    return false;
#endif
}

} // namespace mw
