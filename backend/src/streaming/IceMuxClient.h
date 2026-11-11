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

#include <QJsonObject>
#include <QString>

#include <rtc/rtc.hpp>

#include <functional>
#include <optional>

/// The stream worker's end of the ICE mux (network/IceMux in the server
/// process): the UDP sockets that process holds, opened here from what the
/// config line carries, and for each PeerConnection a feed socket and ICE
/// credentials of its own, announced to the server on stdout.
///
/// One worker, one stream: the handles opened here live as long as the
/// process. docs/design/ice-mux.md
namespace mw::icemuxclient {

using Emit = std::function<void(const QJsonObject&)>;

/// `mux` is the config line's "iceMux": {"hop": port, "sockets": [base64…]}.
/// False (and nothing kept) when it is absent or a socket cannot be opened:
/// the stream then takes a socket of its own, as without the mux.
bool configure(const QJsonObject& mux, Emit tell);
bool available();

/// A new ICE agent on the shared sockets: its feed and its credentials, and
/// the server told about them. Nothing when the feed cannot be made.
std::optional<rtc::IceSharedSockets> openAgent();

/// The agent's peer passed its checks: `candidate` is the selected remote one.
/// From any thread.
void confirmed(const rtc::Candidate& candidate);

/// The agent is over (its PeerConnection closed, or failed).
void closeAgent();

} // namespace mw::icemuxclient
