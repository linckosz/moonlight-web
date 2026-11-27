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

#include "common/Types.h"

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QPair>
#include <QSet>
#include <QString>

/**
 * Origin/CSRF gates for incoming requests.
 *
 * The server grants full admin access to whoever connects from the machine
 * itself (see HttpServer::isLocalRequest). That trust is placed in the source
 * IP alone, which a browser running on the host will happily lend to any page
 * it has open: a malicious site can make the victim's own browser issue the
 * requests, and they arrive from 127.0.0.1 like the real admin UI's would.
 *
 * These helpers separate "a request our frontend made" from "a request some
 * other page made through the victim's browser", using signals a page cannot
 * forge. Kept free of Qt GUI/SSL and of HttpServer state so the whole set is
 * unit-testable on every platform (see tests/test_request_guard.cpp).
 */
namespace RequestGuard {

/// Where a request was initiated from, as far as the browser will tell us.
enum class Initiator
{
    SameOrigin, ///< our own frontend, or a user-typed navigation
    CrossSite,  ///< another page drove this request — never trust it
    Unknown     ///< no browser signals at all (curl, scripts, tunnels)
};

/// Normalize an Origin or Host value to a comparable "host[:port]" form:
/// lowercased, scheme dropped, default port removed, IPv6 brackets stripped.
QString normalizeAuthority(const QString& value);

/// Classify a request from its headers (keys must be lowercase, as
/// HttpParser produces them).
Initiator classifyInitiator(const QMap<QString, QString>& headers);

/// Convenience: true only for requests a foreign page initiated.
bool isCrossSite(const QMap<QString, QString>& headers);

/// Whether a WebSocket upgrade may proceed, given its Origin and Host headers.
/// WebSockets bypass the same-origin policy entirely, so this is the only
/// barrier available for the signaling channel.
bool isWebSocketOriginAllowed(const QString& origin, const QString& hostHeader);

/// Whether @p hostHeader is a name/address this machine legitimately answers
/// to: loopback, a private LAN address, an mDNS name, or @p publicDomain.
/// Anything else means we were reached under a name we do not own — the
/// signature of DNS rebinding — and must not be treated as addressing us.
bool isTrustedHost(const QString& hostHeader, const QString& publicDomain);

/// Whether @p hostHeader can ONLY have been reached from this machine or its
/// LAN: loopback, a private/link-local address, or an mDNS name. The public
/// domain is deliberately excluded, which is the whole point — behind a
/// TLS-terminating tunnel (cloudflared, nport) every visitor on earth reaches
/// us from 127.0.0.1 under that very domain, so pairing "local peer" with
/// "trusted Host" would hand the internet the host machine's privileges.
/// Reaching us under the domain proves nothing about where the caller sits; the
/// host machine proves it there with the host key instead (POST
/// /api/auth/host-key), which is what the frontend already does on the parity
/// redirect.
bool isLocalHostName(const QString& hostHeader);

/// Whether a request carries a header that only a proxy on the way writes
/// (keys lowercase): the forwarding headers of reverse proxies, CDNs and
/// tunnels. The peer of such a request is the proxy, not the browser, so
/// nothing may be granted on the peer's address. It only ever takes away: a
/// caller that forges one loses what its address was worth.
bool forwardedByProxy(const QMap<QString, QString>& headers);

/// Whether a request is this machine reaching itself on loopback and nothing
/// wider: its socket peer is a loopback address (127.0.0.0/8, ::1, or
/// ::ffff:127.x), it asked under a loopback name (localhost, 127.x, ::1), and
/// no proxy on the way forwarded it. The narrowest form of the local privilege
/// (Decision::localPrivilege, which also takes this machine's own LAN
/// addresses and names): a request it holds for has that privilege too. It
/// keeps the host's own plain-HTTP page from being sent to HTTPS
/// (HttpServer::processRequest): a LAN caller that writes "localhost" in Host
/// is sent there like everyone else, and so is a local reverse proxy unless
/// proxiedHttpsOnLoopback() holds for it.
bool loopbackOnly(const QString& peer, const QString& hostHeader,
                  const QMap<QString, QString>& headers);

/// Whether a plain-HTTP request is a reverse proxy on this machine relaying a
/// browser that reached it over HTTPS: its socket peer is a loopback address
/// (127.0.0.0/8, ::1, or ::ffff:127.x), and every protocol the proxy reports
/// is https: each item of X-Forwarded-Proto, and the proto= of each element of
/// Forwarded. At least one of the two is present; a Forwarded element without
/// proto=, an empty item, a header given more than once (repeatedHeaders,
/// lowercase) or any other value answers false. It decides one thing only:
/// such a request is served on the plain port instead of being sent to HTTPS
/// (HttpServer::processRequest), which the browser could not reach through
/// the proxy. What the request may do is evaluate()'s, where a proxy header
/// takes the local privilege away (forwardedByProxy).
bool proxiedHttpsOnLoopback(const QString& peer, const QMap<QString, QString>& headers,
                            const QSet<QString>& repeatedHeaders);

/// Whether every protocol a proxy reports for its browser is https: each item
/// of X-Forwarded-Proto, and the proto= of each element of Forwarded. At least
/// one of the two is present; a Forwarded element without exactly one proto=,
/// an empty item, a header given more than once (repeatedHeaders, lowercase)
/// or any other value answers false. No other header counts.
bool proxySaysHttps(const QMap<QString, QString>& headers, const QSet<QString>& repeatedHeaders);

/// proxiedHttpsOnLoopback() for a reverse proxy elsewhere on the local
/// network: the socket peer is a private IPv4 (LanHttp::isPrivateIpv4: 10/8,
/// 172.16/12, 192.168/16, 169.254/16, "::ffff:" taken off; never CGNAT nor
/// IPv6), is not @p gateway (the router, when known; empty: no exclusion), and
/// proxySaysHttps() holds. It decides only that the request is served on the
/// plain port instead of being sent to HTTPS; its rights are a remote
/// browser's (evaluate(): a LAN peer has no local privilege).
bool proxiedHttpsFromLan(const QString& peer, const QMap<QString, QString>& headers,
                         const QSet<QString>& repeatedHeaders, const QString& gateway);

/// Whether a request being sent to HTTPS looks relayed by a proxy on this
/// machine or the local network that does not report HTTPS: a loopback or
/// private IPv4 peer, a proxy header (forwardedByProxy(), X-Forwarded-Ssl or
/// Front-End-Https), and proxySaysHttps() false. Then the server logs what to
/// add, by header names only.
bool proxyHintWanted(const QString& peer, const QMap<QString, QString>& headers,
                     const QSet<QString>& repeatedHeaders);

/// What a request on the plain-HTTP port gets, decided in this order: a TV
/// the owner listed (@p listedTv, LanHttp) keeps its own path; behind a
/// TLS-terminating tunnel (@p tunnelEdge: loopback peer, public Host), this
/// machine on loopback (loopbackOnly()), or a reverse proxy here or on the
/// LAN that reports HTTPS (proxiedHttpsOnLoopback(), proxiedHttpsFromLan()),
/// it is served; anything else is sent to HTTPS. Rights are not decided here.
enum class PlainArrival
{
    LanHttp,
    Served,
    Redirected,
};
PlainArrival plainArrival(bool listedTv, bool tunnelEdge, const QString& peer,
                          const QString& hostHeader, const QMap<QString, QString>& headers,
                          const QSet<QString>& repeatedHeaders, const QString& gateway);

/// The log line for proxyHintWanted(): the peer's address and the NAMES of
/// the proxy headers it carries, never their values, and what to add.
QString proxyHintText(const QString& peer, const QMap<QString, QString>& headers,
                      const QSet<QString>& repeatedHeaders);

/// How often that line is written: once per peer per kWindowMs (the
/// kMaxPeers most recent peers kept), and kGlobalMax lines per kWindowMs for
/// all peers together, then a single Suppressed, then nothing until the
/// window ends. The clock is the caller's (milliseconds).
class ProxyHintLimiter
{
public:
    enum class Verdict
    {
        Write,
        Suppressed,
        Silent,
    };
    static constexpr qint64 kWindowMs = 10 * 60 * 1000;
    static constexpr int kMaxPeers = 32;
    static constexpr int kGlobalMax = 20;
    Verdict take(const QString& peer, qint64 nowMs);

private:
    QList<QPair<QString, qint64>> m_Peers; // oldest first
    qint64 m_WindowStart = -1;
    int m_InWindow = 0;
    bool m_SuppressedSaid = false;
};

/// Whether a request body may be accepted. Requires application/json for any
/// non-empty body, which also forces a CORS preflight on cross-origin writes.
bool isBodyContentTypeAllowed(const QString& contentType, qsizetype bodySize);

// ── Access decision ─────────────────────────────────────────────────────────
// The whole "what is this request allowed to do" question in one pure
// function, so the interaction between the checks (which is where the holes
// hide) is covered by tests rather than by reading processRequest.

/// The parts of an incoming request the decision looks at.
struct Request
{
    QString method;
    QString path;
    QMap<QString, QString> headers; // lowercase keys
    qsizetype bodySize = 0;
};

/// Narrow a parsed request down to those parts. Production and the TNR both go
/// through this, so a field dropped on the way to evaluate() fails a test
/// instead of silently opening a gate (a lost Sec-Fetch-Site would).
Request describe(const HttpRequest& req);

/// What the server knows about the caller, independent of the request itself.
struct Context
{
    bool peerLocal = false;    ///< socket peer is this machine (HttpServer::isLocalRequest)
    bool hostSession = false;  ///< valid host-key session cookie
    bool adminSession = false; ///< session that unlocked admin with the remote password
    bool adminKeyOk = false;   ///< request carries the current admin key
    bool adminLocked = false;  ///< launched with --noadmin: nobody administers
    QString publicDomain;      ///< the domain we answer to, "" when none
    /// The request came over the local channel (HttpServer::Arrival::Local),
    /// from a program of this install on this machine. Set by the code that
    /// reads that channel and by nothing a request carries.
    bool localChannel = false;
};

enum class Outcome
{
    Allow,
    BlockCrossSite,   ///< 403 — another page initiated this
    BlockContentType, ///< 415 — body is not application/json
};

struct Decision
{
    Outcome outcome = Outcome::BlockCrossSite; ///< fail closed
    bool localPrivilege = false;               ///< may skip the session check
    bool hostMachine = false;                  ///< the caller IS the host machine
    bool adminPrivilege = false;               ///< may use "localhost only" routes
    bool hostTrusted = false;                  ///< Host header names this machine
    bool hostUntrusted = false;                ///< local peer, foreign Host (rebinding)
};

Decision evaluate(const Request& req, const Context& ctx);

/// Whether a request that came over the local channel may be routed at all:
/// what the tray icon, the command line and a second launch ask of the running
/// server, each with its verb, and nothing else. An allow-list, so a route
/// added later stays off this channel until it is named here. Streams, pairing
/// and sessions have no business on it.
bool localChannelRoute(const QString& method, const QString& path);

/// One entry of localChannelRoute()'s list.
struct LocalRoute
{
    const char* method;
    const char* path;
};

/// The list localChannelRoute() reads, as it is: what a test walks, so that a
/// route added to it later is covered by every test of the channel's gate.
QList<LocalRoute> localChannelRoutes();

/// Group attributes a token states, as winnt.h numbers them (checked against
/// it on Windows, LocalChannel.cpp), so the verdict can be tested everywhere.
inline constexpr quint32 kGroupEnabled = 0x00000004;  ///< SE_GROUP_ENABLED
inline constexpr quint32 kGroupDenyOnly = 0x00000010; ///< SE_GROUP_USE_FOR_DENY_ONLY
/// SECURITY_MANDATORY_MEDIUM_RID: the level of a program a person started.
inline constexpr quint32 kMediumIntegrity = 0x00002000;

/// Who the local channel lets in, from what its server end read of the caller.
enum class LocalVerdict
{
    Admit,
    StandardAccount, ///< read, and neither the server's account nor an administrator
    Sandboxed,       ///< below medium integrity, an AppContainer, or a restricted token
    Unreadable,      ///< the caller's token could not be read in full
};

/// The verdict on a caller of the local channel (Windows: what LocalChannel
/// read of its token). Closed whenever in doubt:
///   - nothing of the caller read → Unreadable;
///   - a token below medium integrity, an AppContainer or a restricted one →
///     Sandboxed, whoever it belongs to: a browser's renderer is the person's
///     own account and an administrator's too;
///   - the server's own account (@p serverSid) → Admit;
///   - for a server that runs as a service (@p service), a member of the
///     administrators (@p administratorsSid) → Admit, whether the group is
///     enabled or there for deny only, as UAC leaves it in an administrator's
///     unelevated programs: an administrator of this PC may already act as
///     the system. Nobody else.
/// Every SID here is the text one function writes (LocalChannel's sidText),
/// compared with itself, never with text from elsewhere.
LocalVerdict localCallerVerdict(bool service, const QString& serverSid,
                                const QString& administratorsSid, const LocalCaller& caller);

/// What the local channel answers before any route of it runs.
enum class LocalGate
{
    Pass,      ///< the caller is let in and the route is one of the channel's
    Forbidden, ///< 403 for any route: the caller is not let in
    NotFound,  ///< 404: let in, but not a route of the channel
};

/// The local channel's one gate, in its order: who asks first, then what.
/// A caller that is not let in is answered 403 whatever it asks, a route of
/// the channel or not, so nothing about the routes is told to it.
LocalGate localChannelGate(const QString& method, const QString& path, LocalVerdict verdict);

/// The HTTP status a refused outcome must be answered with, 0 for Allow. Here
/// rather than at the call site so the status is part of the tested contract.
int blockStatus(Outcome outcome);

/// The machine-readable `error` field that goes with blockStatus(), "" for
/// Allow.
QString blockError(Outcome outcome);

/// How to answer GET /api/admin/token.
enum class AdminTokenReply
{
    Grant, ///< 200 with the key — the caller may use the admin routes
    Empty, ///< 200 with no key — a valid session that simply is not admin
    Deny,  ///< 403, and report an auth failure — no session at all
};

/// The frontend asks for the admin key on every page load, before knowing
/// whether this browser is entitled to one. Only a caller with no session at
/// all is fishing for it: answering Deny to a merely non-admin session would
/// feed ConnectionGuard and eventually ban a legitimate remote user from their
/// own server.
AdminTokenReply adminTokenReply(const Decision& decision, const Context& ctx, bool authenticated);

/// Whether --noadmin refuses this request outright. adminPrivilege already
/// closes every "localhost only" route; these are the ones that change what a
/// booth offers without being admin routes: every change to the host list
/// (add, scan, remove, rename, pair, restart its service, set up its backend,
/// its seats — anything but waking a host and playing on it), the
/// self-update, letting a new device or guest in (PIN login, a device's QR code,
/// share slots, a guest's PIN), the admin unlock and the system actions. shareAllowed
/// (share_in_booth_mode, issue #40) lets the share slots and a guest's PIN
/// through, and nothing else.
///
/// boothPinAllowed (booth_stream_pin) opens the stream PIN's routes,
/// /api/booth/*, which are shut otherwise whatever the verb. It replaces the
/// share links rather than adding to them: no link is minted or read again and
/// no guest's PIN is asked, while what the one streaming does to a guest who
/// joined by the PIN (level, name, removal) goes on — each of those routes
/// checks who asks.
bool lockedByNoAdmin(const QString& method, const QString& path, bool shareAllowed = false,
                     bool boothPinAllowed = false);

/// Whether a session arrives over a tunnel whose browser key is not the one it
/// was paired with (HttpServer::tunnelKeyRefused): @p boundKey is the session's
/// key, empty for a session older than key binding. A request off the tunnel
/// carries no key and is never refused here.
bool tunnelKeyRefused(bool viaTunnel, const QByteArray& tunnelKey, const QByteArray& boundKey);

/// Whether the key a request presents is @p expected, in a time that depends
/// only on the longer of the two and never on where they first differ. Keys
/// of different lengths never match, and an empty @p expected matches nothing
/// (HttpServer::adminKeyMatches fails closed on it).
bool secretMatches(const QByteArray& given, const QByteArray& expected);

} // namespace RequestGuard
