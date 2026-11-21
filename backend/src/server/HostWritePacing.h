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

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QtGlobal>

#include <chrono>
#include <functional>
#include <optional>

/// How often a host write may be made by someone who is not the administrator.
/// Every paired device may scan, wake, restart and add hosts; this keeps any one
/// of them from doing it in a loop. The administrator is never held back, except
/// that a restart they make opens the window for everyone else too.
///
/// One per server, on the HTTP thread: no locking.
class HostWritePacing
{
public:
    using Clock = std::function<qint64()>; // milliseconds, monotonic

    static constexpr qint64 kRestartMs = 60 * 1000;
    static constexpr qint64 kScanMs = 30 * 1000;
    static constexpr qint64 kWakeMs = 10 * 1000;
    static constexpr qint64 kAddPerSessionMs = 10 * 1000;
    static constexpr qint64 kAddWindowMs = 60 * 60 * 1000;
    static constexpr int kAddsPerWindow = 30;
    /// What a refused add is told to wait while another one runs.
    static constexpr qint64 kAddBusyMs = 5 * 1000;

    explicit HostWritePacing(Clock clock = steadyMs)
        : m_Clock(std::move(clock))
    {}

    qint64 now() const { return m_Clock(); }

    // ── Restart ──────────────────────────────────────────────────────────────

    /// How long @p uuid still may not be restarted by this caller; 0 when it may.
    qint64 restartWaitMs(const QString& uuid, bool admin) const
    {
        if (admin) return 0;
        const auto it = m_Restarted.constFind(uuid);
        if (it == m_Restarted.constEnd()) return 0;
        const qint64 left = *it + kRestartMs - now();
        return left > 0 ? left : 0;
    }

    /// A restart of @p uuid was accepted: the window opens now. Returns what it
    /// replaced, for restartUndo.
    std::optional<qint64> restartBegin(const QString& uuid)
    {
        std::optional<qint64> previous;
        const auto it = m_Restarted.constFind(uuid);
        if (it != m_Restarted.constEnd()) previous = *it;
        m_Restarted[uuid] = now();
        return previous;
    }

    /// The restart was turned down before anything was done (no such host, no
    /// way to restart it): the window is as it was.
    void restartUndo(const QString& uuid, std::optional<qint64> previous)
    {
        if (previous)
            m_Restarted[uuid] = *previous;
        else
            m_Restarted.remove(uuid);
    }

    // ── Scan ─────────────────────────────────────────────────────────────────

    /// Whether this scan is made. Not the administrator's: one in kScanMs for
    /// the whole machine, counted when it is made.
    bool scanAllowed(bool admin)
    {
        if (admin) return true;
        const qint64 t = now();
        if (m_LastScan && t - *m_LastScan < kScanMs) return false;
        m_LastScan = t;
        return true;
    }

    // ── Wake-on-LAN ──────────────────────────────────────────────────────────

    /// The answer of a packet sent to @p uuid less than kWakeMs ago, to give
    /// again without sending another.
    std::optional<QJsonObject> wakeReplay(const QString& uuid) const
    {
        const auto it = m_Woken.constFind(uuid);
        if (it == m_Woken.constEnd() || now() - it->at >= kWakeMs) return std::nullopt;
        return it->answer;
    }

    void wakeSent(const QString& uuid, const QJsonObject& answer)
    {
        m_Woken[uuid] = {now(), answer};
    }

    // ── Adding a host by address ─────────────────────────────────────────────

    /// A non-administrator's attempt to add a host, by session @p sessionId.
    /// Returns 0 and counts the attempt — whatever comes of it — when it may
    /// go ahead (call addEnd once it is answered); otherwise how long to wait.
    qint64 addBegin(const QString& sessionId)
    {
        const qint64 t = now();
        if (m_AddInFlight) return kAddBusyMs;

        while (!m_Adds.isEmpty() && t - m_Adds.first() >= kAddWindowMs)
            m_Adds.removeFirst();
        if (m_Adds.size() >= kAddsPerWindow) return m_Adds.first() + kAddWindowMs - t;

        for (auto it = m_AddBySession.begin(); it != m_AddBySession.end();) {
            if (t - it.value() >= kAddPerSessionMs)
                it = m_AddBySession.erase(it);
            else
                ++it;
        }
        const auto mine = m_AddBySession.constFind(sessionId);
        if (mine != m_AddBySession.constEnd()) return *mine + kAddPerSessionMs - t;

        m_AddBySession[sessionId] = t;
        m_Adds.append(t);
        m_AddInFlight = true;
        return 0;
    }

    void addEnd() { m_AddInFlight = false; }

    static qint64 steadyMs()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

private:
    struct Woken
    {
        qint64 at = 0;
        QJsonObject answer;
    };

    Clock m_Clock;
    QHash<QString, qint64> m_Restarted;
    std::optional<qint64> m_LastScan;
    QHash<QString, Woken> m_Woken;
    QHash<QString, qint64> m_AddBySession;
    QList<qint64> m_Adds;
    bool m_AddInFlight = false;
};
