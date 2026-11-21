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
#include <QStringList>
#include <QtGlobal>

#include <chrono>
#include <functional>

/// Whose a host's pairing is. A pairing belongs to the session that started it:
/// only that session is shown its PIN and may drive it, until it leaves it
/// alone for kIdleMs or the session itself is gone. Then another may start
/// afresh — but never while a chain is in flight, since its callbacks still
/// hold the NvPairingManager.
///
/// Keyed by host uuid. Holds session ids, never a session's token. Pure, with
/// the time handed in, so the tests drive it.
class PairingOwners
{
public:
    static constexpr qint64 kIdleMs = 5 * 60 * 1000;

    using SessionAlive = std::function<bool(const QString& sessionId)>;

    enum class Start
    {
        Fresh,     ///< nothing to keep: start a new pairing
        ResumeOwn, ///< the caller's own chain is in flight: same PIN again
        Replace,   ///< free the pairing there and start a new one
        Busy,      ///< someone else's, or not freeable yet: 409, no PIN
    };

    void begin(const QString& uuid, const QString& sessionId, qint64 now)
    {
        m_Owners[uuid] = {sessionId, now};
    }

    void clear(const QString& uuid) { m_Owners.remove(uuid); }

    bool has(const QString& uuid) const { return m_Owners.contains(uuid); }

    bool isOwner(const QString& uuid, const QString& sessionId) const
    {
        const auto it = m_Owners.constFind(uuid);
        return it != m_Owners.constEnd() && !sessionId.isEmpty() && it->sessionId == sessionId;
    }

    /// The owner did something with it: the idle clock starts again.
    void touch(const QString& uuid, qint64 now)
    {
        auto it = m_Owners.find(uuid);
        if (it != m_Owners.end()) it->lastSeen = now;
    }

    /// Left alone too long, or its session is gone.
    bool expired(const QString& uuid, qint64 now, const SessionAlive& alive) const
    {
        const auto it = m_Owners.constFind(uuid);
        if (it == m_Owners.constEnd()) return true;
        if (now - it->lastSeen >= kIdleMs) return true;
        return alive && isSessionId(it->sessionId) && !alive(it->sessionId);
    }

    /// A caller that is not a session carries a "kind:" id ("peer:<address>",
    /// "local:provisioning"); a session id is a hash, with no colon. Only a
    /// session can be gone.
    static bool isSessionId(const QString& id) { return !id.contains(QLatin1Char(':')); }

    /// What a pair/start by @p sessionId does to the pairing of @p uuid.
    /// @p active: a pairing is held for that host; @p inFlight: its chain runs.
    Start decideStart(const QString& uuid, bool active, bool inFlight, const QString& sessionId,
                      bool admin, qint64 now, const SessionAlive& alive) const
    {
        if (!active) return Start::Fresh;
        if (isOwner(uuid, sessionId)) return inFlight ? Start::ResumeOwn : Start::Replace;
        if (!admin && !expired(uuid, now, alive)) return Start::Busy;
        // The administrator's, or one nobody drives any more: replaced, but not
        // under a chain that still runs.
        return inFlight ? Start::Busy : Start::Replace;
    }

    /// Whether a pair (submit) by @p sessionId may touch the pairing of @p uuid.
    /// A host nobody is pairing answers as it does today.
    bool maySubmit(const QString& uuid, const QString& sessionId) const
    {
        return !has(uuid) || isOwner(uuid, sessionId);
    }

    static qint64 steadyMs()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    /// The hosts whose pairing nobody drives any more.
    QStringList expiredHosts(qint64 now, const SessionAlive& alive) const
    {
        QStringList out;
        for (auto it = m_Owners.constBegin(); it != m_Owners.constEnd(); ++it)
            if (expired(it.key(), now, alive)) out.append(it.key());
        return out;
    }

private:
    struct Owner
    {
        QString sessionId;
        qint64 lastSeen = 0;
    };
    QMap<QString, Owner> m_Owners;
};
