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

#include <array>
#include <cstdint>

/// Where the listeners go when the well-known port is out of reach — typically
/// an unprivileged desktop install (AppImage, `./MoonlightWeb` from a build
/// tree) where bind(80) and bind(443) are denied outright; only the packaged
/// systemd unit gets CAP_NET_BIND_SERVICE.
///
/// The ladder exists because the port it lands on is PERSISTED and becomes the
/// next boot's preferred port, so it is the address the user types, the one in
/// the tray tooltip and the one written into the Desktop shortcut. It has to be
/// the same tomorrow.
///
/// That rules out the whole high range the fallback used to scan (49443+,
/// 49080+): those ports sit inside the OS dynamic/ephemeral range — 32768-60999
/// on Linux (net.ipv4.ip_local_port_range), 49152-65535 on Windows — where an
/// outgoing connection this very process makes (STUN, ACME, Sunshine, WebRTC)
/// can be holding the port at the moment of the next bind. The listener then
/// moves, gets persisted, and the address drifts on every restart.
///
/// Every candidate below is < 32768 and > 1023: never handed out as an
/// ephemeral port on either OS, and bindable without privileges. Fixed values,
/// tried in order, so a second instance on the same machine takes the next rung
/// deterministically instead of racing for one.
namespace PortFallback {

/// HTTPS listener candidates, in order, when the preferred port is unavailable.
inline constexpr std::array<uint16_t, 3> kHttps{8443, 18443, 28443};

/// HTTP listener candidates, in order, when the preferred port is unavailable.
inline constexpr std::array<uint16_t, 3> kHttp{8080, 18080, 28080};

/// True when @p port can never be stolen by an outgoing connection: outside the
/// dynamic range of both Linux (32768-60999) and Windows (49152-65535), and
/// outside the privileged range no unelevated process can bind.
inline constexpr bool isStable(uint16_t port)
{
    return port > 1023 && port < 32768;
}

} // namespace PortFallback
