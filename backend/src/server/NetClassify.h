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

/// Single source of truth for "where did this peer come from".
///
/// Two near-identical classifiers used to live in the tree and had already
/// drifted apart: SignalingServer::isPrivateAddress() knew about 169.254/16,
/// the clientIsLocal lambda in main.cpp did not. Neither knew about the CGNAT
/// range, which is what a mesh VPN peer (Tailscale and friends) arrives from —
/// so such a peer was classified as an internet client, its host candidate was
/// rewritten to the public IP, and the tunnel path was silently destroyed.
namespace NetClassify {

enum class Kind {
    Loopback, ///< 127.0.0.0/8, ::1 — this machine.
    Private,  ///< RFC1918, link-local, IPv6 ULA — same LAN.
    Tunnel,   ///< 100.64.0.0/10. Carrier-grade NAT range, also what mesh VPNs
              ///< (Tailscale, Headscale) hand out. Not routable on the public
              ///< internet, and the peer is inside the operator's own network.
    Public,   ///< Everything else — a genuine internet peer.
};

/// Classify a peer address. Accepts IPv4, IPv6, IPv4-mapped IPv6
/// (`::ffff:192.168.1.2`) and the literal "localhost". An address that does not
/// parse is treated as Public: the conservative answer, since every caller uses
/// the non-Public verdicts to grant something.
inline Kind classify(const QString& ip)
{
    if (ip.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0) return Kind::Loopback;

    // Strip the IPv4-mapped IPv6 prefix so the IPv4 range checks below see a
    // plain address. Qt reports these as IPv6Protocol, but toIPv4Address()
    // still yields the embedded IPv4 — a tunnel relaying IPv6 to loopback
    // would otherwise be misread as an internet peer.
    QString bare = ip;
    if (bare.startsWith(QLatin1String("::ffff:"), Qt::CaseInsensitive)) bare = bare.mid(7);

    const QHostAddress addr(bare);
    if (addr.isNull()) return Kind::Public;
    if (addr.isLoopback()) return Kind::Loopback;

    bool isIpv4 = false;
    const quint32 v4 = addr.toIPv4Address(&isIpv4);
    if (isIpv4) {
        if ((v4 & 0xFF000000u) == 0x0A000000u) return Kind::Private; // 10.0.0.0/8
        if ((v4 & 0xFFF00000u) == 0xAC100000u) return Kind::Private; // 172.16.0.0/12
        if ((v4 & 0xFFFF0000u) == 0xC0A80000u) return Kind::Private; // 192.168.0.0/16
        if ((v4 & 0xFFFF0000u) == 0xA9FE0000u) return Kind::Private; // 169.254.0.0/16
        if ((v4 & 0xFFC00000u) == 0x64400000u) return Kind::Tunnel;  // 100.64.0.0/10
        return Kind::Public;
    }

    if (addr.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR raw = addr.toIPv6Address();
        if ((raw[0] & 0xFE) == 0xFC) return Kind::Private;                    // fc00::/7 ULA
        if (raw[0] == 0xFE && (raw[1] & 0xC0) == 0x80) return Kind::Private;  // fe80::/10
    }

    return Kind::Public;
}

/// True when the address is not routable on the public internet, **excluding**
/// the tunnel range. This is the historical isPrivateAddress() semantics, kept
/// verbatim for the ICE-server decision: a mesh VPN peer still wants STUN, so
/// its srflx address remains available if the tunnel path fails.
inline bool isPrivateOrLoopback(Kind k)
{
    return k == Kind::Loopback || k == Kind::Private;
}

/// True when the peer sits inside a network the operator controls — this
/// machine, the LAN, or their own mesh VPN. Such a peer may be shown our
/// internal host candidates; a Public peer may not, or the LAN topology leaks.
inline bool isTrustedPeer(Kind k)
{
    return k != Kind::Public;
}

/// Parse back what toString() produced. Used to carry a classification across
/// the process boundary into a stream worker: the worker only ever sees the
/// loopback end of the local WebSocket proxy, so it cannot classify the real
/// browser itself and must be told.
inline Kind fromString(const QString& s, Kind fallback = Kind::Public)
{
    if (s == QLatin1String("loopback")) return Kind::Loopback;
    if (s == QLatin1String("private")) return Kind::Private;
    if (s == QLatin1String("tunnel")) return Kind::Tunnel;
    if (s == QLatin1String("public")) return Kind::Public;
    return fallback;
}

/// Convenience for logging.
inline const char* toString(Kind k)
{
    switch (k) {
    case Kind::Loopback: return "loopback";
    case Kind::Private: return "private";
    case Kind::Tunnel: return "tunnel";
    case Kind::Public: return "public";
    }
    return "unknown";
}

} // namespace NetClassify
