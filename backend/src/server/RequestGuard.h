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

#include <QMap>
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
/// signature of DNS rebinding — and must not grant the local privilege.
bool isTrustedHost(const QString& hostHeader, const QString& publicDomain);

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

/// What the server knows about the caller, independent of the request itself.
struct Context
{
    bool peerLocal = false;    ///< socket peer is this machine (HttpServer::isLocalRequest)
    bool hostSession = false;  ///< valid host-key session cookie
    bool adminSession = false; ///< session that unlocked admin with the remote password
    bool adminKeyOk = false;   ///< request carries the current admin key
    QString publicDomain;      ///< the domain we answer to, "" when none
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

} // namespace RequestGuard
