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

#include <QAbstractSocket>
#include <QHostAddress>
#include <QString>
#include <QUdpSocket>

#include <cstdint>
#include <exception>

namespace mw::relaystart {

/// Whether a UDP port can be bound right now. A pinned port libdatachannel
/// cannot bind leaves the session with no candidate at all, so the pin is only
/// taken when this succeeds.
inline bool udpPortFree(uint16_t port)
{
    QUdpSocket probe;
    const bool ok = probe.bind(QHostAddress::AnyIPv4, port, QAbstractSocket::DontShareAddress);
    probe.close();
    return ok;
}

/// Whether this stream can use the router's hole, which leads to one port only.
/// A second stream on the machine can hold it: the library then binds nothing,
/// and the hole leads to someone else anyway.
inline bool holeUsable(bool hasHole, uint16_t port)
{
    return hasHole && udpPortFree(port);
}

/// Runs a relay's prepare(), which can throw: the first data channel makes the
/// offer and gathers on the spot, and gathering throws when no socket can be
/// bound. Nothing above it catches, so it would end the whole worker. False, with
/// the reason, when it threw.
template <typename Prepare> bool prepareCaught(Prepare&& prepare, QString& why)
{
    try {
        prepare();
        return true;
    } catch (const std::exception& e) {
        why = QString::fromUtf8(e.what());
        return false;
    }
}

} // namespace mw::relaystart
