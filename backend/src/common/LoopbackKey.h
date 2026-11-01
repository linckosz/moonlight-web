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

#include <QByteArray>

class QWebSocket;

/**
 * The key of the server's internal WebSockets.
 *
 * Signaling, the stream relay and the control channel listen on 127.0.0.1, and
 * are reached through HttpServer's proxy and ControlTunnel. Those two carry this
 * key on the upgrade; the sockets take nothing else. One key per server run,
 * handed to its workers with the rest of their configuration.
 */
namespace mw::loopback {

inline constexpr char kHeader[] = "X-MW-Internal-Key";

/// This process's key: a worker's from setKey(), otherwise drawn at first use.
QByteArray key();
void setKey(const QByteArray& key);

/// The upgrade request carrying `key`, in place of any such header it came with.
/// An upgrade that does not parse as one is returned as it is, and is refused.
QByteArray stampUpgrade(const QByteArray& upgradeRequest, const QByteArray& key);

/// Whether this accepted socket came with `key`. An empty key accepts nothing.
bool accepts(const QWebSocket& socket, const QByteArray& key);

} // namespace mw::loopback
