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

#include <functional>

class AppSettings;
class QObject;

/**
 * @brief Internet Access turned off, in one place and in one order.
 *
 * Every way of turning it off (the admin's checkbox, POST /api/internet/disable,
 * POST /api/internet/enable with false, the same through --local-request) goes
 * through turnOff(). Off means everything Internet Access opened goes: the
 * switch, what InternetAccessManager runs, every browser tunnel already open
 * with its router hole, the router's holes themselves, and the line to the
 * introduction server.
 *
 * In two steps. At once, before the HTTP answer leaves: the switch is written
 * off, then InternetAccessManager stops. From then on the tunnel takes no new
 * browser (tunnelAdmits()), whatever the line still brings. Then, on the next
 * turn of the event loop and all together: every tunnel peer goes, the router
 * allocator hears the switch, and the line is stopped. Deferred so that the
 * answer to a page reaching this host through a tunnel is sent before that
 * tunnel goes; that page may still not receive it.
 */
namespace mw::internetoff {

struct Steps
{
    /// The switch, written off (AppSettings::setInternetAccessEnabled(false)).
    std::function<void()> writeOff;
    /// InternetAccessManager::stop().
    std::function<void()> stopAccess;
    /// Every browser tunnel goes, open or not (ControlTunnel::dropAllPeers).
    std::function<void(const QString& why)> dropTunnels;
    /// RouterPortAllocator::switchChanged(): its holes go with the switch.
    std::function<void()> switchChanged;
    /// The line to the introduction server is stopped (onInternetAccessToggled(false)).
    std::function<void()> lineOff;
    /// Whether the switch still reads off when the deferred steps run. If it
    /// was turned back on in between, the line is kept (lineOff is skipped);
    /// the tunnels and the allocator's step run either way. Not given: off.
    std::function<bool()> stillOff;
};

/// Turn Internet Access off: writeOff and stopAccess now, the rest together on
/// the next turn of @p context's event loop. @p why names the caller in the log.
void turnOff(const Steps& steps, QObject* context, const QString& why);

/// Whether the tunnel may take a browser, or go on with one: only while
/// Internet Access is on. Read where a browser arrives, where it signals, and
/// where a router hole comes back for one.
bool tunnelAdmits(const AppSettings& settings);

} // namespace mw::internetoff
