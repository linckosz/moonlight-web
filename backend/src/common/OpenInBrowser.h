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

#include "common/DetachedProcess.h"

#include <QDesktopServices>
#include <QString>
#include <QUrl>

namespace mw {

/// Open a link in the user's default browser: the first-run page, the tray's
/// menu entries, a second launch.
///
/// On Linux, xdg-open is started here, through mw::startDetached, so it gets
/// none of our descriptors. It is also the path a working .desktop launcher
/// takes: QDesktopServices::openUrl goes through the XDG desktop portal, which
/// under Ubuntu's snap-packaged Firefox (the distro default) can pop a blank
/// window that never receives the URL. When xdg-open is missing (a minimal
/// desktop), Qt's opener is the fallback.
///
/// Elsewhere Qt's opener is used. On Windows it asks the shell from this
/// process, and a program the shell starts that way receives none of our
/// handles (test_child_stdio). On macOS, LaunchServices starts the browser,
/// not this process.
///
/// A self-signed localhost certificate still shows a one-time browser warning
/// the user must accept: that is inherent to self-signed TLS.
inline void openInBrowser(const QString& url)
{
#if defined(Q_OS_LINUX)
    if (mw::startDetached(QStringLiteral("xdg-open"), {url})) return;
#endif
    QDesktopServices::openUrl(QUrl(url));
}

} // namespace mw
