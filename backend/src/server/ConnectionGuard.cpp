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

#include "ConnectionGuard.h"

#include <QDateTime>
#include <QHostAddress>

bool ConnectionGuard::isExempt(const QString& ip)
{
    if (ip.isEmpty()) return false;

    // Strip IPv4-mapped IPv6 prefix and any IPv6 scope id.
    QString h = ip;
    if (h.startsWith("::ffff:", Qt::CaseInsensitive)) h = h.mid(7);
    int pct = h.indexOf('%');
    if (pct >= 0) h = h.left(pct);

    QHostAddress addr(h);
    if (addr.isNull()) return false;
    if (addr.isLoopback()) return true;

    if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
        quint32 v = addr.toIPv4Address();
        if ((v & 0xFF000000) == 0x0A000000) return true; // 10.0.0.0/8
        if ((v & 0xFFF00000) == 0xAC100000) return true; // 172.16.0.0/12
        if ((v & 0xFFFF0000) == 0xC0A80000) return true; // 192.168.0.0/16
    } else if (addr.protocol() == QAbstractSocket::IPv6Protocol) {
        Q_IPV6ADDR v6 = addr.toIPv6Address();
        if (v6[0] == 0xFE && (v6[1] & 0xC0) == 0x80) return true; // fe80::/10 link-local
        if ((v6[0] & 0xFE) == 0xFC) return true;                  // fc00::/7 ULA
    }
    return false;
}

void ConnectionGuard::trimWindow(std::deque<qint64>& q, qint64 nowMs, qint64 windowMs)
{
    const qint64 cutoff = nowMs - windowMs;
    while (!q.empty() && q.front() < cutoff)
        q.pop_front();
}

bool ConnectionGuard::allowConnection(const QString& ip)
{
    return allowConnection(ip, QDateTime::currentMSecsSinceEpoch());
}

bool ConnectionGuard::allowConnection(const QString& ip, qint64 nowMs)
{
    if (isExempt(ip)) return true;

    IpState& s = m_state[ip];
    s.lastSeenMs = nowMs;

    if (nowMs < s.bannedUntilMs) return false;

    s.connTimes.push_back(nowMs);
    trimWindow(s.connTimes, nowMs, CONN_WINDOW_MS);

    if (static_cast<int>(s.connTimes.size()) > CONN_MAX_PER_WINDOW) {
        s.bannedUntilMs = nowMs + BAN_MS;
        s.connTimes.clear();
        return false;
    }
    return true;
}

void ConnectionGuard::reportAuthFailure(const QString& ip)
{
    reportAuthFailure(ip, QDateTime::currentMSecsSinceEpoch());
}

void ConnectionGuard::reportAuthFailure(const QString& ip, qint64 nowMs)
{
    if (isExempt(ip)) return;

    IpState& s = m_state[ip];
    s.lastSeenMs = nowMs;

    s.authFailTimes.push_back(nowMs);
    trimWindow(s.authFailTimes, nowMs, AUTHFAIL_WINDOW_MS);

    if (static_cast<int>(s.authFailTimes.size()) >= AUTHFAIL_MAX) {
        s.bannedUntilMs = nowMs + BAN_MS;
        s.authFailTimes.clear();
    }
}

bool ConnectionGuard::isBanned(const QString& ip) const
{
    return isBanned(ip, QDateTime::currentMSecsSinceEpoch());
}

bool ConnectionGuard::isBanned(const QString& ip, qint64 nowMs) const
{
    auto it = m_state.constFind(ip);
    if (it == m_state.constEnd()) return false;
    return nowMs < it->bannedUntilMs;
}

int ConnectionGuard::banSecondsRemaining(const QString& ip, qint64 nowMs) const
{
    auto it = m_state.constFind(ip);
    if (it == m_state.constEnd() || nowMs >= it->bannedUntilMs) return 0;
    return static_cast<int>((it->bannedUntilMs - nowMs + 999) / 1000);
}

void ConnectionGuard::purge()
{
    purge(QDateTime::currentMSecsSinceEpoch());
}

void ConnectionGuard::purge(qint64 nowMs)
{
    for (auto it = m_state.begin(); it != m_state.end();) {
        const bool banned = nowMs < it->bannedUntilMs;
        const bool idle = (nowMs - it->lastSeenMs) > IDLE_PURGE_MS;
        if (!banned && idle)
            it = m_state.erase(it);
        else
            ++it;
    }
}
