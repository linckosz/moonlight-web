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

#include <QDateTime>
#include <QList>
#include <QLocale>
#include <QPair>
#include <QString>

#include "server/StreamActivity.h"

/**
 * @brief What the tray says about the people watching this screen.
 *
 * Decided here rather than in TrayManager so that it can be tested without a
 * desktop. Three answers come out of the same polls:
 *
 *  - The notices: one per person, never lost. While Windows says it would not
 *    show one (a full-screen game, a presentation, quiet hours), a notice WAITS,
 *    and once it would, they go out one at a time. A person who came and went
 *    while it waited is told as "from HH:MM to HH:MM" rather than not at all:
 *    the person at this screen was busy, not absent, and that is precisely when
 *    being watched unseen matters.
 *  - Whether anyone is watching at all: the red dot on the icon. It rests on the
 *    viewer list alone, never on what Windows says about notices, and counts
 *    every viewer, this machine's own browser included — "self" is decided on a
 *    session flag that a device on the LAN can also hold, so it names a viewer
 *    and hides nothing.
 *  - The recent viewers, for the tray menu. In this process's memory only: no
 *    file, no setting, no route, and gone with the process.
 *
 * Bounded both ways by people, not events: one waiting notice and one recent
 * line per person, whose repeat visits merge into it, and @ref kMaxPeople of
 * each. A browser connecting and leaving in a loop can neither flood the
 * notification area nor grow this process.
 */
class ViewerNotices
{
public:
    /// People kept, in the queue and in the recent list. The oldest go first.
    static constexpr int kMaxPeople = 20;
    /// Between two notices: a newer balloon can replace the one on screen.
    static constexpr qint64 kGapMs = 6000;

    /// Whether Windows would show a notice now, from SHQueryUserNotificationState.
    ///
    /// Only QUNS_ACCEPTS_NOTIFICATIONS shows. Every other state waits —
    /// QUNS_NOT_PRESENT, QUNS_BUSY, QUNS_RUNNING_D3D_FULL_SCREEN,
    /// QUNS_PRESENTATION_MODE, QUNS_QUIET_TIME, QUNS_APP, and any value this
    /// code has never heard of — and so does a call that failed: a notice that
    /// waits a little too long is the cheaper of the two mistakes.
    static bool windowsShows(bool callSucceeded, int state);

    /// A device's name is cut to this many characters (an ellipsis follows).
    static constexpr int kMaxNameLength = 48;

    /// What a viewer is called in a notice and in the menu: their device's own
    /// name, cleaned (see the .cpp), "This computer" for this machine's browser.
    static QString nameOf(const StreamViewer& v);

    /// One recent viewer: the first time this process saw them arrive, and the
    /// last time they left (invalid while they are still watching).
    struct Viewing
    {
        QString id;
        QString name;
        QDateTime from;
        QDateTime to;
    };

    /// What one poll changed, for the caller's log.
    struct Changes
    {
        QList<StreamViewer> arrived;
        QList<StreamViewer> left;
    };

    /// One poll's viewer list. The first one describes a world that already
    /// existed (a tray started after the streams it finds): it is adopted,
    /// announced to nobody. After that, every arrival and departure updates the
    /// recent list, and — when @p announce, the owner's "tell me" switch — the
    /// queue of notices.
    Changes observe(const QList<StreamViewer>& viewers, const QDateTime& now, bool announce);

    /// A notice is waiting and the gap since the last one has passed: the caller
    /// asks Windows, then calls take() if it would show it, hold() if not.
    bool due(qint64 nowMs) const;
    /// The next notice's text, out of the queue. @p nowMs starts the gap.
    QString take(const QDateTime& now, qint64 nowMs);
    /// Windows would not show it now. Every waiting notice is marked as having
    /// waited, which changes how it is told. True when that is news (something
    /// was not marked yet), so that the caller logs it once, not every poll.
    bool hold();
    /// The next notice has waited for Windows at least once.
    bool nextWasHeld() const;

    int waiting() const { return m_Queue.size(); }
    /// People the queue dropped to stay within @ref kMaxPeople, since the start.
    int dropped() const { return m_Dropped; }

    /// Anyone is watching this screen right now: the red dot.
    bool watched() const { return !m_Current.isEmpty(); }
    /// Newest activity first, at most @ref kMaxPeople.
    QList<Viewing> recent() const { return m_Recent; }
    /// One recent viewer as the menu says it.
    QString recentLine(const Viewing& v, const QDateTime& now) const;

    /// Clock times are written in this locale (the system's by default).
    void setLocale(const QLocale& locale) { m_Locale = locale; }

private:
    struct Pending
    {
        QString id;
        QString name;
        QDateTime from;
        QDateTime to;             // invalid while they are still watching
        bool arrivalTold = false; // only the departure is left to tell
        bool held = false;        // Windows made it wait at least once
    };

    void queueArrival(const StreamViewer& v, const QDateTime& now);
    void queueDeparture(const StreamViewer& v, const QDateTime& since, const QDateTime& now);
    void noteArrival(const StreamViewer& v, const QDateTime& now);
    void noteDeparture(const StreamViewer& v, const QDateTime& since, const QDateTime& now);
    QString text(const Pending& p, const QDateTime& now) const;
    QString clock(const QDateTime& t, const QDateTime& now) const;

    QList<StreamViewer> m_Current;            // the previous poll
    QList<QPair<QString, QDateTime>> m_Since; // when each current viewer arrived
    bool m_Known = false;
    QList<Pending> m_Queue;  // oldest first
    QList<Viewing> m_Recent; // newest first
    qint64 m_LastToldMs = -1;
    int m_Dropped = 0;
    QLocale m_Locale;
};
