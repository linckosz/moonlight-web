/*
 * Moonlight-Web — browser-based Sunshine/GameStream client.
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

#include <QHash>
#include <QString>
#include <deque>

/**
 * In-process per-IP abuse mitigation — the "fail2ban" equivalent for the
 * application layer (fail2ban itself is Linux-only; this works on Windows).
 *
 * Two independent sliding-window detectors, both feeding a single temporary
 * ban list checked at accept() time so a banned IP is dropped before any TLS
 * handshake or request parsing cost:
 *
 *   1. Connection flood   — too many new TCP connections per IP per window.
 *      Note: the server closes the socket after every response
 *      (Connection: close), so a single legitimate page load is already a
 *      burst of dozens of connections from one IP. The threshold is therefore
 *      generous; it targets true floods, not normal browsing.
 *
 *   2. Auth-failure flood — too many rejected requests (401) per IP per window,
 *      i.e. credential scanning / brute force.
 *
 * Loopback and private (RFC 1918 / IPv6 ULA / link-local) addresses are fully
 * exempt and never tracked — they are trusted LAN clients.
 *
 * Single-threaded: only ever touched from the HttpServer/SslServer event loop
 * (main thread), so no locking is required. Time is injectable on every method
 * for deterministic unit testing.
 */
class ConnectionGuard
{
public:
    ConnectionGuard() = default;

    /// Record a new connection from @p ip and decide whether to accept it.
    /// Returns false when the IP is banned or has just crossed the connection
    /// flood threshold (which also arms a fresh ban). Call at accept() time.
    bool allowConnection(const QString& ip);
    bool allowConnection(const QString& ip, qint64 nowMs);

    /// Record a rejected/unauthorized request (HTTP 401). Repeated failures
    /// within the window ban the IP. Call when a remote request is refused.
    void reportAuthFailure(const QString& ip);
    void reportAuthFailure(const QString& ip, qint64 nowMs);

    /// True while @p ip is within an active ban.
    bool isBanned(const QString& ip) const;
    bool isBanned(const QString& ip, qint64 nowMs) const;

    /// Seconds remaining on an IP's ban, or 0 if not banned.
    int banSecondsRemaining(const QString& ip, qint64 nowMs) const;

    /// Drop entries with no recent activity and no active ban. Call periodically
    /// to bound memory under a churn of unique source IPs.
    void purge();
    void purge(qint64 nowMs);

    int trackedIpCount() const { return m_state.size(); }

    /// True for loopback / RFC 1918 / IPv6 ULA / link-local addresses, which are
    /// exempt from all limiting. Static so it can be reused by callers.
    static bool isExempt(const QString& ip);

    // ── Tunables (public so tests and callers can reference them) ────────────
    static constexpr qint64 CONN_WINDOW_MS = 10'000;     // 10 s
    static constexpr int CONN_MAX_PER_WINDOW = 200;      // generous: page loads burst
    static constexpr qint64 AUTHFAIL_WINDOW_MS = 60'000; // 60 s
    static constexpr int AUTHFAIL_MAX = 10;              // brute-force scanning
    static constexpr qint64 BAN_MS = 600'000;            // 10 min temporary ban
    static constexpr qint64 IDLE_PURGE_MS = 300'000;     // drop entry after 5 min idle

private:
    struct IpState
    {
        std::deque<qint64> connTimes;     // new-connection timestamps (ms)
        std::deque<qint64> authFailTimes; // 401 timestamps (ms)
        qint64 bannedUntilMs = 0;
        qint64 lastSeenMs = 0;
    };

    /// Drop timestamps older than @p windowMs from the front of @p q.
    static void trimWindow(std::deque<qint64>& q, qint64 nowMs, qint64 windowMs);

    QHash<QString, IpState> m_state;
};
