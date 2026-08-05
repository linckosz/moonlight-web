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

#include <QString>

/**
 * @brief Keeps this machine's display awake so Sunshine always has something to
 *        capture.
 *
 * A blanked output stops producing frames, so the host's capture backend (KMS on
 * Wayland, X11 grab, DXGI, CoreGraphics) has nothing to grab: Sunshine's encoder
 * probe fails and every /launch answers HTTP 503 — the "host can't capture its
 * screen" dialog. The session being *locked* is not the problem; the screen
 * being *off* is. So the fix is to stop the desktop from blanking it, not to try
 * to wake it after the fact (by then the launch has already been refused).
 *
 * Linux/GNOME only, and deliberately so:
 *   - GNOME exposes exactly the two settings that matter through gsettings, and
 *     they cover both its X11 and Wayland sessions;
 *   - we already run inside the user's GUI session, so no pkexec/polkit dance;
 *   - other desktops (KDE, XFCE) keep the same knobs elsewhere, and guessing
 *     wrong would silently do nothing — better to report "unsupported" and let
 *     the user use their own settings app.
 * Windows and macOS report unsupported: the Windows host is configured by its
 * own installer, and macOS needs root (`pmset`) plus a caffeinate-style
 * assertion that outlives us.
 *
 * Screen *locking* is left strictly alone — a locked but lit session streams
 * fine, and unlocking it for the user is not ours to decide. Applying this is
 * opt-in from the setup wizard, and undoing it is a two-click trip to the
 * desktop's own Power settings (which is where it belongs, and where the user
 * will look for it later).
 */
namespace DisplaySleep {

/// Whether this platform/desktop exposes the knobs `keepDisplayAwake()` needs.
/// False → the wizard hides the option entirely.
bool isSupported();

/// Whether the display is already set never to blank and never to sleep on AC.
/// False when unsupported (nothing is known, so nothing is claimed).
bool isDisplayKeptAwake();

/// Set "never blank the screen" + "never sleep on AC" for the current user.
/// Returns an empty string on success, or a human-readable reason on failure.
QString keepDisplayAwake();

} // namespace DisplaySleep
