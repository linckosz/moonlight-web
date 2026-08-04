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

namespace mw {

/**
 * True when a display server is reachable — i.e. Qt can create real windows.
 *
 * Linux: the DISPLAY (X11) / WAYLAND_DISPLAY (Wayland) socket answers a
 * connection. Presence of the variable is not enough: a gamescope session
 * (Bazzite Gaming Mode, SteamOS) exports both before its Xwayland accepts
 * clients, so an autostart launched there sees a DISPLAY that points at
 * nothing. Everywhere else the windowing system is always there.
 *
 * Deliberately blind to MW_SERVICE: this answers "can Qt draw?", which is what
 * picks the QPA platform plugin, and a supervised launch inside a graphical
 * session still has a display.
 *
 * Must stay usable BEFORE QApplication exists (it selects the platform plugin),
 * so it reads the raw environment and touches no Qt GUI type.
 *
 * The answer is computed once and cached, then replaced by the platform plugin
 * that actually loaded (see confirmDisplayServer): the QPA platform is frozen
 * at QApplication construction, so a display appearing later cannot be used by
 * this process anyway, and every caller must agree with what Qt really got.
 */
bool hasDisplayServer();

/**
 * Record what Qt actually managed to load, once QApplication exists: an
 * offscreen platform means no display, whatever the environment claimed.
 *
 * The socket probe above cannot see an X server that accepts the connection and
 * then refuses the handshake (a missing xauth cookie under gamescope). Qt can:
 * its xcb plugin fails to initialize and the QT_QPA_PLATFORM fallback list
 * lands on offscreen. This is how that verdict reaches the tray, the browser
 * auto-open, the Sunshine installer and the `headless` flag in the API.
 */
void confirmDisplayServer(bool present);

/**
 * True when a desktop session can show a browser window, a tray icon or a
 * polkit password prompt: a display server AND not a service/daemon launch
 * (MW_SERVICE, set by the systemd unit, the NSSM service and the Docker image).
 *
 * This is the "headless" predicate every user-facing decision keys off — auto
 * opening the setup page, the tray, and the Sunshine auto-install (which needs
 * pkexec, hence an interactive authentication agent).
 *
 * Do NOT use QSystemTrayIcon::isSystemTrayAvailable() for this: GNOME has no
 * system tray by default, which would wrongly report "headless" on a perfectly
 * ordinary desktop and skip the first-run setup wizard.
 */
bool hasDesktopSession();

} // namespace mw
