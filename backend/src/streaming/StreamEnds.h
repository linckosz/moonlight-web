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
#include <QRandomGenerator>
#include <QString>

#include <algorithm>
#include <iterator>

// How each recent stream ended, kept for the page that lost it.
//
// A stream's WebRTC connection lives in its --stream-worker, so the page only
// ever learns that the link went, never why. And the why decides everything: a
// worker that crashed under a healthy stream is worth starting again at once,
// without sending the viewer back to the host page, while a stream the server
// stopped (Stop, a take-over, a revocation, the owner ending a share) or one
// that ended on its own (the app closed, the link died) ends as it always has.
// The page asks here, by the id /start gave it, before doing either.
//
// Bounded twice, in count and in age: nothing else ever removes an entry. A
// stream still running is never aged out, since its end is still to come.
class StreamEnds
{
public:
    enum class State
    {
        Unknown,
        /// Still streaming, as far as this server knows: the page's link went
        /// and the worker has not (a network cut, seen from the page first).
        Live,
        /// The worker died without saying its session was over, and nobody
        /// asked it to stop.
        Crashed,
        /// The worker ended its session itself: the app closed, the link died
        /// under it, the session was revoked.
        Ended,
        /// The server stopped it: Stop, a take-over, a guest's Leave, the
        /// owner ending a share.
        Stopped,
    };

    struct Entry
    {
        State state = State::Unknown;
        QString hostUuid;
        int slot = -1;
        QString clientUniqueId;
        /// The mw_session behind the stream, empty for a guest or localhost.
        QString sessionToken;
        /// When the state last changed, in ms.
        qint64 atMs = 0;
        /// A stream of the console's sign-in screen (RemoteDesktopReturnPolicy).
        bool signInScreen = false;
    };

    static constexpr int kCapacity = 32;
    static constexpr qint64 kMaxAgeMs = 2 * 60 * 1000;

    /// A fresh id for a stream: 128 random bits, hex. It is the page's only
    /// handle on the stream's end, so it must not be guessable from another's.
    static QString newId()
    {
        quint32 words[4];
        QRandomGenerator::system()->fillRange(words);
        QString id;
        for (quint32 w : words)
            id += QStringLiteral("%1").arg(w, 8, 16, QLatin1Char('0'));
        return id;
    }

    /// How a worker that has just gone ended: crashed only when it said
    /// nothing about its session ending and nobody asked it to stop. A worker
    /// killed after a stop it did not finish in time did not crash.
    static State stateAfterExit(bool endedEvent, bool stopAsked)
    {
        return endedEvent || stopAsked ? State::Ended : State::Crashed;
    }

    static QString name(State s)
    {
        switch (s) {
        case State::Live: return QStringLiteral("live");
        case State::Crashed: return QStringLiteral("crashed");
        case State::Ended: return QStringLiteral("ended");
        case State::Stopped: return QStringLiteral("stopped");
        case State::Unknown: break;
        }
        return QStringLiteral("unknown");
    }

    /// What the page is told of @p e. A stream of the sign-in screen whose
    /// worker went while its session is back on the PC's screen (@p
    /// sessionBackOnScreen, read when asked) was signed in through:
    /// "signed_in", and the page starts the same card again, which now
    /// streams the PC's screen. Read when asked rather than when the worker
    /// went: the session reaches the screen a moment after the sign-in
    /// screen's session ends, the worker with it.
    static QString nameFor(const Entry& e, bool sessionBackOnScreen)
    {
        if (e.signInScreen && sessionBackOnScreen &&
            (e.state == State::Ended || e.state == State::Crashed))
            return QStringLiteral("signed_in");
        return name(e.state);
    }

    /// The stream @p id started streaming, on @p slot of @p hostUuid.
    /// @p signInScreen: of the console's sign-in screen (nameFor).
    void begin(const QString& id, const QString& hostUuid, int slot, const QString& clientUniqueId,
               const QString& sessionToken, qint64 nowMs, bool signInScreen = false)
    {
        if (id.isEmpty()) return;
        Entry entry;
        entry.state = State::Live;
        entry.hostUuid = hostUuid;
        entry.slot = slot;
        entry.clientUniqueId = clientUniqueId;
        entry.sessionToken = sessionToken;
        entry.atMs = nowMs;
        entry.signInScreen = signInScreen;
        m_Entries.insert(id, entry);
        prune(nowMs);
    }

    /// The stream @p id is over, as @p state says. The first word stands: a
    /// stream the server stopped does not become a crash because its worker
    /// was then killed, and an unknown id (a launch that never streamed) is
    /// not recorded at all.
    void end(const QString& id, State state, qint64 nowMs)
    {
        auto it = m_Entries.find(id);
        if (it == m_Entries.end() || it->state != State::Live) return;
        it->state = state;
        it->atMs = nowMs;
        prune(nowMs);
    }

    /// What is known of @p id: Unknown once it has aged out, or was never
    /// recorded.
    Entry find(const QString& id, qint64 nowMs) const
    {
        const auto it = m_Entries.constFind(id);
        if (it == m_Entries.constEnd() || expired(*it, nowMs)) return Entry{};
        return *it;
    }

    int size() const { return m_Entries.size(); }

private:
    static bool expired(const Entry& e, qint64 nowMs)
    {
        return e.state != State::Live && nowMs - e.atMs > kMaxAgeMs;
    }

    void prune(qint64 nowMs)
    {
        for (auto it = m_Entries.begin(); it != m_Entries.end();)
            it = expired(*it, nowMs) ? m_Entries.erase(it) : std::next(it);
        // Over capacity: the oldest end goes first, a running stream last.
        while (m_Entries.size() > kCapacity) {
            m_Entries.erase(std::min_element(m_Entries.begin(), m_Entries.end(),
                                             [](const Entry& a, const Entry& b) {
                                                 const bool aEnded = a.state != State::Live;
                                                 const bool bEnded = b.state != State::Live;
                                                 if (aEnded != bEnded) return aEnded;
                                                 return a.atMs < b.atMs;
                                             }));
        }
    }

    QHash<QString, Entry> m_Entries;
};
