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
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <cmath>
#include <initializer_list>

/**
 * @brief A browser's report of a direct connection that could not be opened.
 *
 * When a tunnel fails, the entry page (bootstrap/v1/tunnel.js, diagMessage)
 * sends its machine a `diag` message over the signalling line: the ICE,
 * connection and gathering states, how long since its answer went out, how many
 * candidates of each kind it sent, how many it had from this host, and its ICE
 * check counters. Issue #48 is why: a car's browser reached the introduction
 * server, and then nothing reached its machine, with nothing on either side to
 * say where it stopped.
 *
 * The message comes from the browser, so nothing of it is written as it came.
 * The line is built from this file's own words only: a state is one of a closed
 * list or "other", a candidate kind is one of the 24 this file knows, and every
 * number is clamped. A key or a value outside those lists is dropped.
 *
 * Kept apart from ControlTunnel, which needs libdatachannel, so it can be tested
 * on its own.
 */
namespace TunnelDiag {

/// Lines written per hour, all sessions together. A browser can only report
/// once per connection, but connections can be opened one after another.
constexpr int kMaxLinesPerWindow = 20;
constexpr qint64 kWindowMs = 60LL * 60 * 1000;

/// Where a count stops; the browser stops at the same place.
constexpr int kCountCap = 999;
/// Ten minutes: no handshake lasts longer than the host's own 40 s deadline.
constexpr int kMsCap = 600000;

namespace detail {

inline QString pick(const QJsonValue& v, std::initializer_list<const char*> known)
{
    const QString s = v.toString();
    for (const char* k : known)
        if (s == QLatin1String(k)) return QString::fromLatin1(k);
    return QStringLiteral("other");
}

inline int count(const QJsonValue& v, int cap)
{
    if (!v.isDouble()) return 0;
    const double d = v.toDouble();
    if (!std::isfinite(d) || d <= 0) return 0;
    return d >= cap ? cap : static_cast<int>(d);
}

} // namespace detail

/// The log line for one report, without the session's name.
inline QString describe(const QJsonObject& msg)
{
    using detail::count;
    using detail::pick;

    const QString why = pick(msg.value(QStringLiteral("why")), {"failed", "deadline"});
    const QString ice =
        pick(msg.value(QStringLiteral("ice")),
             {"new", "checking", "connected", "completed", "failed", "disconnected", "closed"});
    const QString connection =
        pick(msg.value(QStringLiteral("connection")),
             {"new", "connecting", "connected", "disconnected", "failed", "closed"});
    const QString gathering =
        pick(msg.value(QStringLiteral("gathering")), {"new", "gathering", "complete"});
    const int ms = count(msg.value(QStringLiteral("ms")), kMsCap);

    // Walked from this file's lists, never from the browser's keys.
    QStringList sent;
    const QJsonObject cand = msg.value(QStringLiteral("cand")).toObject();
    for (const char* type : {"host", "srflx", "prflx", "relay"})
        for (const char* protocol : {"udp", "tcp"})
            for (const char* family : {"v4", "v6", "mdns"}) {
                const QString kind =
                    QStringLiteral("%1/%2/%3")
                        .arg(QLatin1String(type), QLatin1String(protocol), QLatin1String(family));
                const int n = count(cand.value(kind), kCountCap);
                if (n > 0) sent << QStringLiteral("%1 x%2").arg(kind).arg(n);
            }

    QString line =
        QStringLiteral("the browser found no route (%1) — ice=%2, connection=%3, "
                       "gathering=%4, %5 ms after its answer; sent %6; "
                       "had %7 from this host")
            .arg(why, ice, connection, gathering)
            .arg(ms)
            .arg(sent.isEmpty() ? QStringLiteral("no candidate") : sent.join(QStringLiteral(", ")))
            .arg(count(msg.value(QStringLiteral("recv")), kCountCap));

    const QJsonValue checks = msg.value(QStringLiteral("checks"));
    if (checks.isObject()) {
        const QJsonObject c = checks.toObject();
        line += QStringLiteral("; checks sent %1, answered %2, received %3")
                    .arg(count(c.value(QStringLiteral("sent")), kCountCap))
                    .arg(count(c.value(QStringLiteral("answered")), kCountCap))
                    .arg(count(c.value(QStringLiteral("received")), kCountCap));
    } else {
        line += QStringLiteral("; no check counters");
    }
    return line;
}

/**
 * At most kMaxLinesPerWindow lines an hour, all sessions together, and the
 * reports past that counted rather than written: one line says how many when
 * the hour is over. The hour starts with the first report after a quiet one.
 */
class Limiter
{
public:
    /// Whether a report at `nowMs` may be written. Close the past window first
    /// (closeWindow) so its count is not lost.
    bool admit(qint64 nowMs)
    {
        if (m_Start < 0 || nowMs - m_Start >= kWindowMs) {
            m_Start = nowMs;
            m_Lines = 0;
            m_Ignored = 0;
        }
        if (m_Lines < kMaxLinesPerWindow) {
            ++m_Lines;
            return true;
        }
        ++m_Ignored;
        return false;
    }

    /// The reports ignored in a window that is over by `nowMs`, once: 0 when
    /// the window is still running, or when nothing was ignored, or when it has
    /// already been said.
    int closeWindow(qint64 nowMs)
    {
        if (m_Start < 0 || nowMs - m_Start < kWindowMs) return 0;
        const int n = m_Ignored;
        m_Start = -1;
        m_Lines = 0;
        m_Ignored = 0;
        return n;
    }

    /// How long until the running window is over; 0 when there is none.
    qint64 msLeft(qint64 nowMs) const
    {
        if (m_Start < 0) return 0;
        const qint64 left = m_Start + kWindowMs - nowMs;
        return left > 0 ? left : 0;
    }

    /// Reports ignored so far in the running window.
    int ignored() const { return m_Ignored; }

private:
    qint64 m_Start = -1;
    int m_Lines = 0;
    int m_Ignored = 0;
};

} // namespace TunnelDiag
