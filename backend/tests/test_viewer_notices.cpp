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

// ViewerNotices: what the tray says about the people watching this screen
// (A137) — which Windows states let a notice out, the order notices wait and
// leave in, the red dot, and the recent list's bounds.

#include "test_framework.h"

#include "ViewerNotices.h"
#include "server/StreamActivity.h"

#include <QJsonDocument>

namespace {

const QDateTime kT0(QDate(2026, 11, 12), QTime(14, 0));

StreamViewer viewer(const QString& id, const QString& name, bool self = false)
{
    return StreamViewer{id, name, self};
}

QString at(const QLocale& loc, int h, int m)
{
    return loc.toString(QTime(h, m), QLocale::ShortFormat);
}

// A tray that has already seen an empty room, so the next poll announces.
ViewerNotices started(const QLocale& loc)
{
    ViewerNotices n;
    n.setLocale(loc);
    n.observe({}, kT0, true);
    return n;
}

} // namespace

void run_viewer_notices_tests()
{
    const QLocale loc = QLocale::c();

    SECTION("ViewerNotices: only QUNS_ACCEPTS_NOTIFICATIONS lets a notice out");
    {
        // 1 NOT_PRESENT, 2 BUSY, 3 RUNNING_D3D_FULL_SCREEN, 4 PRESENTATION_MODE,
        // 5 ACCEPTS_NOTIFICATIONS, 6 QUIET_TIME, 7 APP; 0 and 8+ unknown.
        for (int state = -1; state <= 9; ++state)
            CHECK_EQ(ViewerNotices::windowsShows(true, state), state == 5);
        // A failed call holds, whatever it left in the state.
        for (int state = -1; state <= 9; ++state)
            CHECK(!ViewerNotices::windowsShows(false, state));
    }

    SECTION("ViewerNotices: the first poll is adopted, not announced");
    {
        ViewerNotices n;
        n.setLocale(loc);
        const auto c = n.observe({viewer("a", "Leo")}, kT0, true);
        CHECK(c.arrived.isEmpty());
        CHECK_EQ(n.waiting(), 0);
        CHECK(n.watched());
        CHECK_EQ(n.recent().size(), 1);
    }

    SECTION("ViewerNotices: accepted at once, told as it happens");
    {
        ViewerNotices n = started(loc);
        const auto c = n.observe({viewer("a", "Leo")}, kT0.addSecs(60), true);
        CHECK_EQ(c.arrived.size(), 1);
        CHECK(n.due(0));
        CHECK_EQ(n.take(kT0.addSecs(60), 0), QStringLiteral("Leo started streaming this screen"));
        CHECK_EQ(n.waiting(), 0);
        n.observe({}, kT0.addSecs(600), true);
        CHECK(n.due(10000));
        CHECK_EQ(n.take(kT0.addSecs(600), 10000),
                 QStringLiteral("Leo stopped streaming this screen"));
    }

    SECTION("ViewerNotices: a stream that ends while Windows holds is told from-to");
    {
        ViewerNotices n = started(loc);
        n.observe({viewer("a", "Leo")}, kT0.addSecs(2 * 60), true);
        CHECK(n.due(0));
        CHECK(n.hold());  // Windows busy: news, logged once
        CHECK(!n.hold()); // still busy at the next poll: not news
        CHECK(n.nextWasHeld());
        n.observe({}, kT0.addSecs(30 * 60), true); // gone before Windows accepts
        CHECK_EQ(n.waiting(), 1);
        CHECK_EQ(n.take(kT0.addSecs(40 * 60), 0),
                 QStringLiteral("Leo streamed this screen from %1 to %2")
                     .arg(at(loc, 14, 2), at(loc, 14, 30)));
    }

    SECTION("ViewerNotices: still watching when released, told since when");
    {
        ViewerNotices n = started(loc);
        n.observe({viewer("a", "Leo")}, kT0.addSecs(5 * 60), true);
        n.hold();
        CHECK_EQ(n.take(kT0.addSecs(20 * 60), 0),
                 QStringLiteral("Leo has been streaming this screen since %1").arg(at(loc, 14, 5)));
    }

    SECTION("ViewerNotices: a departure held after a told arrival keeps both times");
    {
        ViewerNotices n = started(loc);
        n.observe({viewer("a", "Leo")}, kT0.addSecs(60), true);
        n.take(kT0.addSecs(60), 0); // told at once
        n.observe({}, kT0.addSecs(45 * 60), true);
        n.hold();
        CHECK_EQ(n.take(kT0.addSecs(50 * 60), 60000),
                 QStringLiteral("Leo streamed this screen from %1 to %2")
                     .arg(at(loc, 14, 1), at(loc, 14, 45)));
    }

    SECTION("ViewerNotices: several people, one notice each, in order, 6 s apart");
    {
        ViewerNotices n = started(loc);
        n.observe({viewer("a", "Leo"), viewer("b", "Max")}, kT0.addSecs(60), true);
        n.observe({viewer("a", "Leo"), viewer("b", "Max"), viewer("c", "")}, kT0.addSecs(120),
                  true);
        CHECK_EQ(n.waiting(), 3);
        CHECK(n.due(1000));
        CHECK(n.take(kT0.addSecs(120), 1000).startsWith(QStringLiteral("Leo ")));
        CHECK(!n.due(1000 + ViewerNotices::kGapMs - 1)); // the gap holds the next one
        CHECK(n.due(1000 + ViewerNotices::kGapMs));
        CHECK(n.take(kT0.addSecs(126), 7000).startsWith(QStringLiteral("Max ")));
        CHECK(n.take(kT0.addSecs(132), 13000).startsWith(QStringLiteral("Someone ")));
        CHECK(!n.due(100000));
    }

    SECTION("ViewerNotices: repeat visits merge into the person's one notice");
    {
        ViewerNotices n = started(loc);
        for (int i = 0; i < 50; ++i) { // connect, disconnect, in a loop, while held
            n.observe({viewer("a", "Leo")}, kT0.addSecs(60 + i * 10), true);
            n.hold();
            n.observe({}, kT0.addSecs(65 + i * 10), true);
        }
        CHECK_EQ(n.waiting(), 1);
        CHECK_EQ(n.recent().size(), 1);
        CHECK_EQ(n.take(kT0.addSecs(3600), 0),
                 QStringLiteral("Leo streamed this screen from %1 to %2")
                     .arg(at(loc, 14, 1),
                          loc.toString(kT0.addSecs(65 + 49 * 10).time(), QLocale::ShortFormat)));
    }

    SECTION("ViewerNotices: the queue and the list keep 20 people, oldest dropped first");
    {
        ViewerNotices n = started(loc);
        QList<StreamViewer> room;
        for (int i = 0; i < 25; ++i) {
            room.append(viewer(QStringLiteral("id%1").arg(i), QStringLiteral("P%1").arg(i)));
            n.observe(room, kT0.addSecs(60 + i), true);
            n.hold();
        }
        CHECK_EQ(n.waiting(), ViewerNotices::kMaxPeople);
        CHECK_EQ(n.dropped(), 5);
        CHECK(n.take(kT0.addSecs(3600), 0).startsWith(QStringLiteral("P5 ")));
        CHECK_EQ(n.recent().size(), ViewerNotices::kMaxPeople);
        CHECK_EQ(n.recent().first().name, QStringLiteral("P24"));
        CHECK_EQ(n.recent().last().name, QStringLiteral("P5"));
    }

    SECTION("ViewerNotices: the dot counts every viewer, this machine's own too");
    {
        ViewerNotices n = started(loc);
        CHECK(!n.watched());
        n.observe({viewer("h", "Host machine (remote link)", true)}, kT0.addSecs(60), true);
        CHECK(n.watched());
        CHECK_EQ(n.take(kT0.addSecs(60), 0),
                 QStringLiteral("This computer started streaming this screen"));
        n.observe({}, kT0.addSecs(120), true);
        CHECK(!n.watched()); // gone with the last viewer
    }

    SECTION("ViewerNotices: the owner's switch off queues nothing, the list and dot still follow");
    {
        ViewerNotices n = started(loc);
        n.observe({viewer("a", "Leo")}, kT0.addSecs(60), false);
        CHECK_EQ(n.waiting(), 0);
        CHECK(n.watched());
        CHECK_EQ(n.recent().size(), 1);
    }

    SECTION("ViewerNotices: the menu line, and a name with '&'");
    {
        ViewerNotices n = started(loc);
        n.observe({viewer("a", "Tom & Jerry")}, kT0.addSecs(60), true);
        const QDateTime now = kT0.addSecs(120);
        CHECK_EQ(n.recentLine(n.recent().first(), now),
                 QStringLiteral("Tom && Jerry, since %1").arg(at(loc, 14, 1)));
        n.observe({}, kT0.addSecs(30 * 60), true);
        CHECK_EQ(n.recentLine(n.recent().first(), now),
                 QStringLiteral("Tom && Jerry, %1 to %2").arg(at(loc, 14, 1), at(loc, 14, 30)));
        // Another day's visit carries its date.
        const QDateTime tomorrow = kT0.addDays(1);
        CHECK(n.recentLine(n.recent().first(), tomorrow) !=
              QStringLiteral("Tom && Jerry, %1 to %2").arg(at(loc, 14, 1), at(loc, 14, 30)));
    }

    SECTION("ViewerNotices: a device's own name is cleaned before the tray says it");
    {
        const auto name = [](const QString& s) { return ViewerNotices::nameOf(viewer("x", s)); };
        // Controls, format characters (the bidi override among them) and line
        // breaks go; spaces are trimmed and collapsed.
        const QString rlo(QChar(0x202E)), zwj(QChar(0x200D)), bell(QChar(0x07)), ls(QChar(0x2028));
        CHECK_EQ(name(QStringLiteral("  Pi") + rlo + QStringLiteral("xel") + bell +
                      QStringLiteral("  7\n") + ls + zwj),
                 QStringLiteral("Pixel 7"));
        // Nothing left is no name at all.
        CHECK_EQ(name(rlo + QStringLiteral("  ") + bell), QStringLiteral("Someone"));
        // Long names are cut, with an ellipsis, and never inside a surrogate pair.
        const QString longName = QString(60, QLatin1Char('a'));
        CHECK_EQ(name(longName),
                 QString(ViewerNotices::kMaxNameLength, QLatin1Char('a')) + QChar(0x2026));
        const QString emoji = QString::fromUcs4(U"\U0001F600", 1);
        QString emojis;
        for (int i = 0; i < 60; ++i)
            emojis += emoji;
        const QString cut = name(emojis);
        CHECK_EQ(cut.toUcs4().size(), ViewerNotices::kMaxNameLength + 1);
        CHECK(cut.startsWith(emoji));
        // The tray's own words, whatever the case and spacing, are quoted.
        CHECK_EQ(name(QStringLiteral("This computer")), QStringLiteral("\"This computer\""));
        CHECK_EQ(name(QStringLiteral("this  COMPUTER ")), QStringLiteral("\"this COMPUTER\""));
        CHECK_EQ(name(QStringLiteral("ThisComputer")), QStringLiteral("\"ThisComputer\""));
        CHECK_EQ(name(QStringLiteral("someone")), QStringLiteral("\"someone\""));
        CHECK_EQ(name(QStringLiteral("This computer 2")), QStringLiteral("This computer 2"));
        // This machine's own browser keeps its label, unquoted.
        CHECK_EQ(ViewerNotices::nameOf(viewer("h", "This computer", true)),
                 QStringLiteral("This computer"));
        CHECK_EQ(ViewerNotices::nameOf(viewer("h", "anything", true)),
                 QStringLiteral("This computer"));
    }

    SECTION("StreamActivity: who a slot's viewer is, the server decides");
    {
        const int owner = 2; // slots 0 and 1: the owner's stream and its standby
        const QString tokA = QStringLiteral("tokenA-0123456789abcdef");
        const QString tokB = QStringLiteral("tokenB-fedcba9876543210");
        // Two devices are two viewers, whatever their browsers claim to be: the
        // uniqueid is no input any more.
        const auto two =
            StreamActivity::viewersOf({{0, tokA, "A", false}, {2, tokB, "B", false}}, owner);
        CHECK_EQ(two.size(), 2);
        CHECK(two.at(0).id != two.at(1).id);
        // Both legs of one session are one person, named after the first.
        const auto one =
            StreamActivity::viewersOf({{0, tokA, "A", false}, {1, tokA, "A", false}}, owner);
        CHECK_EQ(one.size(), 1);
        CHECK_EQ(one.first().name, QStringLiteral("A"));
        // The token itself never leaves: a 16-hex hash of it, the same each time.
        const QString id = StreamActivity::viewerId(tokA, 0, owner);
        CHECK_EQ(id.size(), 16);
        CHECK(!id.contains(QStringLiteral("token")));
        CHECK_EQ(StreamActivity::viewerId(tokA, 1, owner), id);
        CHECK(StreamActivity::viewerId(tokB, 0, owner) != id);
        // No session: guests each their own slot, the owner's slots one owner.
        const auto guests = StreamActivity::viewersOf({{0, QString(), "", false},
                                                       {1, QString(), "", false},
                                                       {2, QString(), "g1", false},
                                                       {3, QString(), "g2", false}},
                                                      owner);
        CHECK_EQ(guests.size(), 3);
        CHECK_EQ(guests.at(1).id, QStringLiteral("slot-2"));
        CHECK_EQ(guests.at(2).id, QStringLiteral("slot-3"));
        // What the snapshot carries to the tray client: the hash, not the token.
        StreamActivity a;
        a.viewers = one;
        const QByteArray wire = QJsonDocument(a.toJson()).toJson();
        CHECK(!wire.contains("tokenA"));
        CHECK(wire.contains(id.toLatin1()));
    }
}
