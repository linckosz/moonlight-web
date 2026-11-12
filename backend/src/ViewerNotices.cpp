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

#include "ViewerNotices.h"

#include <QCoreApplication>

namespace {

// The tray's own context: these are its strings, moved here to be testable.
QString tr(const char* text)
{
    return QCoreApplication::translate("TrayManager", text);
}

// shellapi.h's QUERY_USER_NOTIFICATION_STATE, the one value that shows.
constexpr int kQunsAcceptsNotifications = 5;

bool hasId(const QList<StreamViewer>& list, const QString& id)
{
    for (const StreamViewer& v : list)
        if (v.id == id) return true;
    return false;
}

// Case and spaces aside, the way a label is compared with a name.
QString folded(const QString& s)
{
    QString out = s.toCaseFolded();
    out.removeIf([](QChar c) { return c.isSpace(); });
    return out;
}

} // namespace

bool ViewerNotices::windowsShows(bool callSucceeded, int state)
{
    return callSucceeded && state == kQunsAcceptsNotifications;
}

// The device name from the sessions table, except for a browser on this very
// machine: its row reads "Host machine (remote link)", which tells one row from
// another there and says nothing to somebody reading a notice on that machine.
// And "Someone" when there is no name at all.
//
// That name is whatever the paired device called itself, so it is cleaned
// before the tray shows it: no control or format characters (a bidi override
// would turn the rest of the notice around), no line or paragraph breaks, at
// most kMaxNameLength characters. And a device that named itself after one of
// the tray's own words is quoted, or "This computer started streaming this
// screen" could be a remote browser's doing.
QString ViewerNotices::nameOf(const StreamViewer& v)
{
    if (v.self) return tr("This computer");

    QString kept;
    for (const char32_t c : v.name.toUcs4()) {
        switch (QChar::category(c)) {
        case QChar::Other_Control:
        case QChar::Other_Format:
        case QChar::Other_Surrogate:
        case QChar::Separator_Line:
        case QChar::Separator_Paragraph: continue;
        default: kept += QString::fromUcs4(&c, 1);
        }
    }
    kept = kept.simplified();
    if (kept.isEmpty()) return tr("Someone");

    const QList<uint> points = kept.toUcs4();
    if (points.size() > kMaxNameLength)
        kept =
            QString::fromUcs4(reinterpret_cast<const char32_t*>(points.constData()), kMaxNameLength)
                .trimmed() +
            QChar(0x2026); // …

    const QString f = folded(kept);
    for (const char* own : {"This computer", "Someone"})
        if (f == folded(tr(own)) || f == folded(QString::fromLatin1(own)))
            return QLatin1Char('"') + kept + QLatin1Char('"');
    return kept;
}

ViewerNotices::Changes ViewerNotices::observe(const QList<StreamViewer>& viewers,
                                              const QDateTime& now, bool announce)
{
    Changes changes;
    if (!m_Known) {
        for (const StreamViewer& v : viewers) {
            m_Since.append({v.id, now});
            noteArrival(v, now);
        }
        m_Current = viewers;
        m_Known = true;
        return changes;
    }

    for (const StreamViewer& v : viewers) {
        if (hasId(m_Current, v.id)) continue;
        changes.arrived.append(v);
        m_Since.append({v.id, now});
        noteArrival(v, now);
        if (announce) queueArrival(v, now);
    }
    // Departures are named from the previous poll, the only place a viewer who
    // is gone still has a name.
    for (const StreamViewer& v : m_Current) {
        if (hasId(viewers, v.id)) continue;
        changes.left.append(v);
        QDateTime since = now;
        for (int i = 0; i < m_Since.size(); ++i) {
            if (m_Since.at(i).first != v.id) continue;
            since = m_Since.takeAt(i).second;
            break;
        }
        noteDeparture(v, since, now);
        if (announce) queueDeparture(v, since, now);
    }
    m_Current = viewers;
    return changes;
}

// A person already waiting in the queue keeps their one place in it: whatever
// they did since, the notice will tell it from their first time to their last.
void ViewerNotices::queueArrival(const StreamViewer& v, const QDateTime& now)
{
    for (Pending& p : m_Queue) {
        if (p.id != v.id) continue;
        p.name = nameOf(v);
        p.to = QDateTime();
        p.arrivalTold = false; // back before their departure was told: one span
        return;
    }
    m_Queue.append({v.id, nameOf(v), now, QDateTime(), false, false});
    while (m_Queue.size() > kMaxPeople) {
        m_Queue.removeFirst();
        ++m_Dropped;
    }
}

void ViewerNotices::queueDeparture(const StreamViewer& v, const QDateTime& since,
                                   const QDateTime& now)
{
    for (Pending& p : m_Queue) {
        if (p.id != v.id) continue;
        p.name = nameOf(v);
        p.to = now;
        return;
    }
    m_Queue.append({v.id, nameOf(v), since, now, true, false});
    while (m_Queue.size() > kMaxPeople) {
        m_Queue.removeFirst();
        ++m_Dropped;
    }
}

void ViewerNotices::noteArrival(const StreamViewer& v, const QDateTime& now)
{
    for (int i = 0; i < m_Recent.size(); ++i) {
        if (m_Recent.at(i).id != v.id) continue;
        Viewing w = m_Recent.takeAt(i);
        w.name = nameOf(v);
        w.to = QDateTime();
        m_Recent.prepend(w);
        return;
    }
    m_Recent.prepend({v.id, nameOf(v), now, QDateTime()});
    while (m_Recent.size() > kMaxPeople)
        m_Recent.removeLast();
}

void ViewerNotices::noteDeparture(const StreamViewer& v, const QDateTime& since,
                                  const QDateTime& now)
{
    for (int i = 0; i < m_Recent.size(); ++i) {
        if (m_Recent.at(i).id != v.id) continue;
        Viewing w = m_Recent.takeAt(i);
        w.name = nameOf(v);
        w.to = now;
        m_Recent.prepend(w);
        return;
    }
    // Pushed out of the list while watching: their visit, at least, comes back.
    m_Recent.prepend({v.id, nameOf(v), since, now});
    while (m_Recent.size() > kMaxPeople)
        m_Recent.removeLast();
}

bool ViewerNotices::due(qint64 nowMs) const
{
    return !m_Queue.isEmpty() && (m_LastToldMs < 0 || nowMs - m_LastToldMs >= kGapMs);
}

QString ViewerNotices::take(const QDateTime& now, qint64 nowMs)
{
    if (m_Queue.isEmpty()) return QString();
    m_LastToldMs = nowMs;
    return text(m_Queue.takeFirst(), now);
}

bool ViewerNotices::hold()
{
    bool news = false;
    for (Pending& p : m_Queue) {
        if (p.held) continue;
        p.held = true;
        news = true;
    }
    return news;
}

bool ViewerNotices::nextWasHeld() const
{
    return !m_Queue.isEmpty() && m_Queue.first().held;
}

// Told as it happened when it went out at once; with its times when it waited,
// because by then "started" or "stopped" would be news of an unknown moment.
QString ViewerNotices::text(const Pending& p, const QDateTime& now) const
{
    if (p.to.isValid()) {
        if (p.arrivalTold && !p.held) return tr("%1 stopped streaming this screen").arg(p.name);
        return tr("%1 streamed this screen from %2 to %3")
            .arg(p.name, clock(p.from, now), clock(p.to, now));
    }
    if (p.held)
        return tr("%1 has been streaming this screen since %2").arg(p.name, clock(p.from, now));
    return tr("%1 started streaming this screen").arg(p.name);
}

// The time alone today; the date too for an older one (a server up for days).
QString ViewerNotices::clock(const QDateTime& t, const QDateTime& now) const
{
    if (t.date() == now.date()) return m_Locale.toString(t.time(), QLocale::ShortFormat);
    return m_Locale.toString(t, QLocale::ShortFormat);
}

// A menu entry: '&' doubled, or a name holding one would lose it to a shortcut.
QString ViewerNotices::recentLine(const Viewing& v, const QDateTime& now) const
{
    QString name = v.name;
    name.replace(QLatin1Char('&'), QStringLiteral("&&"));
    if (v.to.isValid()) return tr("%1, %2 to %3").arg(name, clock(v.from, now), clock(v.to, now));
    return tr("%1, since %2").arg(name, clock(v.from, now));
}
