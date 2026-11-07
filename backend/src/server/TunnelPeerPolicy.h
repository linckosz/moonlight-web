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
#include <QStringList>
#include <QtGlobal>

#include <vector>

/**
 * @brief Which browser tunnels outlive the introduction server's word.
 *
 * A browser's tunnel is a direct connection between the two machines. The
 * introduction server only made the introduction, and once the channel is
 * open nothing it does is needed any more. But its `close` for a session was
 * taken as the browser leaving, and the tunnel was taken down with it — and the
 * server says `close` for reasons that have nothing to do with the browser: a
 * restart of the proxy in front of it closes every socket one by one, and a
 * browser's that closes before its host's line is reported as a departure.
 * Every session then ended on the server's way out, streams on the machine's
 * own LAN included.
 *
 * So a `close` on a tunnel whose channel is open DETACHES it instead, when the
 * browser is one this machine has reason to keep: on its LAN, by the address
 * ICE settled on — never by anything the browser said — or a device that has
 * already carried a signed-in or invited request. A stranger on the internet is
 * taken down as before. Nothing about privilege changes: every request on a
 * detached tunnel is judged exactly as it was, on the same ICE address.
 *
 * What a detached tunnel costs is resources the server no longer counts, so
 * there is a ceiling, and the tunnel still ends the ordinary ways: the channel
 * or DTLS closing (a tab closed) or ICE consent running out (a browser that
 * vanished, 30 s).
 *
 * Kept apart from ControlTunnel, which needs libdatachannel, so the decisions
 * can be tested on their own.
 */
namespace TunnelPeerPolicy {

/// From the server announcing a browser to its channel opening. The bootstrap
/// gives up at 25 s (CONNECT_TIMEOUT_MS in bootstrap/v1/tunnel.js); past this,
/// nobody is waiting for the connection, and it would hold its router hole
/// until the host restarts.
constexpr int kHandshakeDeadlineMs = 40000;

/// Detached tunnels held at once.
constexpr int kMaxDetached = 8;

/// Whether a `close` from the server leaves this tunnel up.
inline bool keepOnClose(bool channelOpen, bool onLan, bool knownCaller)
{
    return channelOpen && (onLan || knownCaller);
}

/// One detached tunnel, as the ceiling sees it.
struct Detached
{
    QString id;
    /// Order of detachment: smaller is older.
    quint64 order = 0;
    /// It has carried a signed-in or invited request.
    bool known = false;
};

/// The tunnel to let go when there are more than kMaxDetached, or an empty id
/// when there are not: the oldest one no request ever vouched for, else the
/// oldest of all.
inline QString evictOne(const std::vector<Detached>& detached)
{
    if (static_cast<int>(detached.size()) <= kMaxDetached) return {};
    const Detached* oldestUnknown = nullptr;
    const Detached* oldest = nullptr;
    for (const Detached& d : detached) {
        if (!oldest || d.order < oldest->order) oldest = &d;
        if (!d.known && (!oldestUnknown || d.order < oldestUnknown->order)) oldestUnknown = &d;
    }
    return oldestUnknown ? oldestUnknown->id : oldest->id;
}

/// The bench key "debug_tunnel_public_address" (AppSettings): "a.b.c.d:port",
/// an IPv4 address and a port from 1 to 65535. False for anything else, which
/// leaves the key off: a mistyped value must never pin a port by accident.
inline bool parsePublicAddress(const QString& value, QString* outIp, quint16* outPort)
{
    const QString v = value.trimmed();
    const int colon = v.lastIndexOf(QLatin1Char(':'));
    if (colon <= 0 || colon == v.size() - 1) return false;
    const QString ip = v.left(colon);
    const QStringList parts = ip.split(QLatin1Char('.'));
    if (parts.size() != 4) return false;
    for (const QString& part : parts) {
        bool ok = false;
        const int n = part.toInt(&ok);
        if (!ok || part.isEmpty() || part.size() > 3 || n < 0 || n > 255) return false;
    }
    bool ok = false;
    const int port = v.mid(colon + 1).toInt(&ok);
    if (!ok || port < 1 || port > 65535) return false;
    *outIp = ip;
    *outPort = static_cast<quint16>(port);
    return true;
}

} // namespace TunnelPeerPolicy
