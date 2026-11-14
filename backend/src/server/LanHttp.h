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
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

/**
 * Plain HTTP for the devices the owner listed (issue #42).
 *
 * A TV browser that refuses a self-signed certificate can still use this
 * server, over http:// on the local network, when its address is in
 * `lan_http_clients`. Everything else on the HTTP port is redirected to HTTPS,
 * as it always was.
 *
 * What travels in clear can be read by anyone on the network, and changed by
 * anyone who can pose as the TV. So what such a device is given is narrow, and
 * every limit is decided here, free of sockets, so that it is tested as it is
 * (tests/test_lan_http.cpp):
 *   - who arrives this way: an IPv4 address of the list, private, never this
 *     machine's own (IPv4 only in this version);
 *   - under which name: an IPv4 address of this machine, or exactly its
 *     ".local" name, on the HTTP port; anything else is not ours to answer;
 *   - from which page: an Origin equal to that name, required on every write
 *     and on the WebSocket;
 *   - what it may ask: the short list of routes below, and nothing a later
 *     route adds unless it is named here;
 *   - with which key: the `mw_lan_http` cookie, read on this path and on no
 *     other, while the HTTPS cookies are never read on this one;
 *   - which host: this machine's own, on Windows, and no other (hostAllowed).
 */
namespace LanHttp {

/// The cookie a device on plain HTTP holds. Never `Secure`: the browser would
/// refuse it over http://. Never read on HTTPS, a tunnel or the local channel.
inline constexpr const char* kCookie = "mw_lan_http";

/// The address as a dotted IPv4, or empty. "::ffff:a.b.c.d" is folded to
/// a.b.c.d; any other IPv6 address, a zone, or something that is not an
/// address gives empty: this version serves IPv4 only.
QString normalizePeer(const QString& address);

/// Whether a dotted IPv4 is of the local network: 10/8, 172.16/12,
/// 192.168/16 or link-local 169.254/16. Loopback, carrier-grade NAT (where
/// mesh VPNs live) and public addresses are not.
bool isPrivateIpv4(const QString& ipv4);

/// The entries of `lan_http_clients` that are honoured: each one normalized,
/// private, and not one of @p ownAddresses (another account of this very
/// machine must never arrive as "the TV"). What is left out goes to
/// @p dropped, as it was written, for one warning.
QSet<QString> usableClients(const QStringList& entries, const QSet<QString>& ownAddresses,
                            QStringList* dropped = nullptr);

/// Whether a peer arrives on plain HTTP as a listed device. @p peer is the
/// socket's address as Qt gives it; it is normalized here, and this machine's
/// own addresses never are admitted, whatever the list says.
bool admits(const QString& peer, const QSet<QString>& usable, const QSet<QString>& ownAddresses);

/// The Host header, when it names this machine as a listed device may reach
/// it: one of its IPv4 addresses (@p ownIpv4), or exactly @p localName
/// ("<machine>.local"), with @p httpPort. The answer is the authority as an
/// Origin would spell it ("192.168.1.10:8080", no port for 80), lowercased;
/// empty when the Host is anything else.
QString validatedHost(const QString& hostHeader, const QSet<QString>& ownIpv4,
                      const QString& localName, quint16 httpPort);

/// Whether @p origin is exactly "http://" + @p validatedHost. An empty Origin
/// and "null" are refused: unlike the HTTPS path, this one asks for it.
bool originAllowed(const QString& origin, const QString& validatedHost);

/// The routes a device on plain HTTP may ask, by method and exact path, one
/// "*" standing for one segment. Everything else answers 404. Static files
/// (any path outside /api/) are read with GET or HEAD.
bool route(const QString& method, const QString& path);

/// Whether this server lets a device on plain HTTP stream at all: on Windows,
/// where this machine's own host holds such a stream to the user's unlocked
/// desktop (native-host core/DesktopGate.h). Elsewhere it reaches no host.
#ifdef Q_OS_WIN
inline constexpr bool kStreamsThisPc = true;
#else
inline constexpr bool kStreamsThisPc = false;
#endif

/// Whether @p path names a host, "/api/hosts/<id>" and below; @p id gets its
/// id, empty when the segment is.
bool namesHost(const QString& path, QString* id = nullptr);

/// Whether a device on plain HTTP may reach the host the server knows by
/// @p id, of backend @p backendType: this machine's own host (@p nativeId, of
/// backend @p nativeType), on a server that streams it (@p streamsThisPc),
/// and no other. Known by its id AND its backend, never by an address: a
/// Sunshine on 127.0.0.1 or on this machine's own LAN address is another
/// host. The lock state another server's screen is in is not this one's to
/// read, and a TV's key, which travels in clear, never drives it.
bool hostAllowed(const QString& id, const QString& backendType, const QString& nativeId,
                 const QString& nativeType, bool streamsThisPc);

/// /api/hosts as a device on plain HTTP reads it: the hosts it may reach
/// (hostAllowed, on each entry's "uuid" and "backendType") and no other, not
/// even their names or addresses.
QJsonArray hostsFor(const QJsonArray& hosts, const QString& nativeId, const QString& nativeType,
                    bool streamsThisPc);

/// The stream socket a device on plain HTTP may open: the signalling one of a
/// slot, "/ws" (slot 0) or "/wsN". Never the legacy video relay
/// ("/ws/stream", "/wsN/stream"), never the control channel. -1 for anything
/// else.
int webSocketSlot(const QString& path);

/// The Content-Security-Policy of an answer on this path: the page may open a
/// WebSocket back to the name it was served under, over ws://, and to nothing
/// else. No HSTS and no upgrade-insecure-requests go with it.
QString securityPolicy(const QString& validatedHost);

/// The cookie that signs a device in, or refreshes it.
QString cookie(const QString& token, qint64 maxAgeSecs);

/// The cookie that signs it out.
QString clearedCookie();

/// The value of `mw_lan_http` in a Cookie header, or empty.
QString tokenFromCookieHeader(const QString& cookieHeader);

/// A User-Agent longer than this is not searched for a model (deviceName).
constexpr qsizetype kMaxUserAgentLength = 512;

/// What a TV is called once let in: the model its browser gives in
/// @p userAgent ("MiTV-MOSR4" out of "Linux; Android 11; MiTV-MOSR4 Build/…"),
/// else its @p address. Only a plain model is taken: 2 to 40 letters, digits,
/// spaces, dots, dashes or underscores, which leaves out Chrome's stand-in "K".
/// Nor is a User-Agent past kMaxUserAgentLength read. A name given in the
/// devices' list replaces it.
QString deviceName(const QString& userAgent, const QString& address);

/// Whether @p name says no more than @p address: none, the address itself, or
/// "Browser at <address>", what a TV was called before deviceName(). The tray
/// then names it by its address, once.
bool namesOnlyAddress(const QString& name, const QString& address);

/// The routes as named for route(), one "METHOD /path" each, in order: what a
/// test compares with a list of its own, so that an addition shows in it.
QStringList routeTable();

/// The routes a TV asks before it is signed in: whether it is, and to be let
/// in. Every other route of the list wants its key.
bool openRoute(const QString& path);

/// What the TV's key is worth on a request.
enum class Key
{
    None,        ///< no mw_lan_http, or two of them
    Valid,       ///< a live session, from the address it is bound to
    Expired,     ///< a session no more (out of time, revoked, unknown)
    OtherAddress ///< a TV's session, asked from another address
};

/// What serveLanHttp does with a request.
enum class Verdict
{
    Serve,
    NotFound,     ///< not a route of the list: 404
    Forbidden,    ///< another page, or a body that is not JSON: 403
    Unauthorized, ///< a route that wants the key, without it: 401
    OtherHost     ///< a route naming a host other than this machine's own: 403
};

struct Gate
{
    Verdict verdict = Verdict::NotFound;
    /// Fed to the connection guard. Not for a GET without a good key: a TV
    /// whose session ran out asks /api/hosts every few seconds, and must not
    /// be banned from the route that lets it in again.
    bool counted = false;
    /// The Cookie header the request goes on with: its mw_lan_http alone, so
    /// that no route reads a mw_session or a mw_player sent in clear.
    QString cookieHeader;
};

/// The decision serveLanHttp applies, without a socket: @p guardRefused is
/// RequestGuard::evaluate's refusal (another site, a body that is not JSON);
/// @p hostAllowed says whether a host a route names may be reached
/// (hostAllowed, with what the server knows of that host). Empty: none may.
Gate gate(const QString& method, const QString& path, const QMap<QString, QString>& headers,
          const QString& validatedHost, Key key, bool guardRefused,
          const std::function<bool(const QString& hostId)>& hostAllowed = {});

struct WsGate
{
    int status = 0;       ///< 0: open; otherwise the refusal's status
    bool counted = false; ///< fed to the connection guard
    int slot = -1;        ///< with 0: the slot whose signalling it opens
};

/// The decision upgradeLanHttp applies: the Origin, required and exact; the
/// path, a slot's signalling; the key; and the slot, its own stream's
/// (@p ownSlot, asked only with a good key).
WsGate webSocketGate(const QString& origin, const QString& validatedHost, const QString& path,
                     Key key, const std::function<bool(int slot)>& ownSlot);

/// A path as a log line may show it: a segment of lower-case letters and
/// dashes (1 to 24) as it is, any other as "*". A request id, a host's id or a
/// file name never reaches the log.
QString pathForLog(const QString& path);

} // namespace LanHttp
