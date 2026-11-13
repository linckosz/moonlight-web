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

#include "server/LanHttp.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>

/**
 * @brief Who is streaming THIS machine's screen right now.
 *
 * Only the native host counts. A session this MoonlightWeb runs against a
 * Sunshine somewhere else is this machine acting as a client — nobody is
 * looking at this screen, and saying so in a notification would be a lie.
 *
 * It is carried as a snapshot rather than a signal because the tray that shows
 * it usually lives in ANOTHER PROCESS: on Windows the server is a service in
 * session 0 and the icon belongs to a small client that decorates it over
 * loopback (runTrayClient). One shape, one comparison, both modes.
 */
struct StreamViewer
{
    /// Stable while the stream lasts, and different for each viewer: what tells
    /// "someone else joined" from "the same person is still here". Decided by
    /// the server (StreamActivity::viewerId), never by the browser: an id a
    /// client picks could be one already watching, and its owner would then
    /// never be listed.
    QString id;
    /// What to call them in a notification — the device name from the admin
    /// sessions list, or the player's row name for an invited guest. May be
    /// empty, and the tray then says "Someone".
    QString name;
    /// The viewer is a browser on this very machine, opened through the tray's
    /// own link. Its row is named "Host machine (remote link)" in the sessions
    /// table, which is the right label THERE — it tells one row from another —
    /// and a strange thing to be told on the machine it names. The tray says
    /// "This computer" instead.
    bool self = false;
    /// The stream this line stands for, by an id made for this one stream and
    /// gone with it (instanceId): what the tray's Disconnect names. Never a
    /// token, a slot or an address, which outlive a stream or pass to another.
    QString instance;
    /// Where it stands, as the tray may show it (shownAddress): a private IPv4,
    /// or "Internet" for any other. Empty for this computer and when unknown.
    QString address;
    /// A TV on plain HTTP (issue #42): said by the server's own session, never
    /// by anything the client sent.
    bool lanHttp = false;
    /// The pool slot of its first stream, for the log line of a Disconnect.
    int slot = -1;
};

/// One live slot of the native host, as far as "who is watching" needs it.
struct StreamSlotFacts
{
    int index = 0;        ///< its place in the pool, the owner's slots first
    QString sessionToken; ///< the signed-in device behind it; empty for none
    QString name;         ///< what to call it (sessions table, or a guest's row)
    bool self = false;    ///< a browser on this very machine
    QString streamId;     ///< the stream's own id (StreamEnds), 128 random bits
    QString address;      ///< where its session stands, as the server knows it
    bool lanHttp = false; ///< a TV on plain HTTP, from its session
};

struct StreamActivity
{
    /// Who a slot's viewer is. A signed-in device is its session: both legs of
    /// the owner's stream (slot 0 and its standby) carry the same token, so a
    /// quality change is the same person continuing. The token itself never
    /// leaves the server — this snapshot goes to the tray client over loopback —
    /// only a hash of it, salted for this one use. A guest has a slot of its
    /// own; the owner's slots without a session (a stream from localhost) are
    /// the owner, once.
    static QString viewerId(const QString& sessionToken, int index, int ownerSlots)
    {
        if (!sessionToken.isEmpty()) {
            const QByteArray h = QCryptographicHash::hash(
                QByteArrayLiteral("viewer|") + sessionToken.toUtf8(), QCryptographicHash::Sha256);
            return QString::fromLatin1(h.toHex().left(16));
        }
        if (index < ownerSlots) return QStringLiteral("owner-local");
        return QStringLiteral("slot-%1").arg(index);
    }

    /// The id of one stream for the tray's Disconnect: its slot and its own
    /// id, hashed with @p salt, which this process draws once. A new stream in
    /// the same slot is another id, so a Disconnect asked from a menu opened
    /// before it never ends it. Empty without a salt or a stream id.
    static QString instanceId(const QByteArray& salt, int index, const QString& streamId)
    {
        if (salt.isEmpty() || streamId.isEmpty()) return {};
        const QByteArray h = QCryptographicHash::hash(salt + '|' + QByteArray::number(index) + '|' +
                                                          streamId.toUtf8(),
                                                      QCryptographicHash::Sha256);
        return QString::fromLatin1(h.toHex().left(16));
    }

    /// An address as the tray may show it: a private IPv4 as it is, anything
    /// else (a public address, IPv6, a tunnel's) as "Internet". A public
    /// address is a person's, and every account of this PC can read the tray.
    static QString shownAddress(const QString& address)
    {
        if (address.isEmpty()) return {};
        const QString v4 = LanHttp::normalizePeer(address);
        return !v4.isEmpty() && LanHttp::isPrivateIpv4(v4) ? v4 : QStringLiteral("Internet");
    }

    /// Who may ask for a stream to end from the tray (issue #42): the local
    /// channel (the icon of a server that runs as a service), or a request with
    /// the administrator's right (@p isLocal, its key included) from a host or
    /// administrator session. Never a TV on plain HTTP, never a page on this
    /// machine without a session, never under --noadmin, whose tray changes
    /// nothing.
    static bool mayDisconnect(bool localChannel, bool isLocal, bool lanHttp, bool hostSession,
                              bool adminSession, bool adminLocked)
    {
        if (adminLocked || lanHttp) return false;
        if (localChannel) return true;
        return isLocal && (hostSession || adminSession);
    }

    /// The people behind the live slots: one viewer per id, named after the
    /// first of their slots, and standing for that slot's stream (@p salt:
    /// instanceId's).
    static QList<StreamViewer> viewersOf(const QList<StreamSlotFacts>& live, int ownerSlots,
                                         const QByteArray& salt = QByteArray())
    {
        QList<StreamViewer> out;
        QSet<QString> seen;
        for (const StreamSlotFacts& s : live) {
            const QString id = viewerId(s.sessionToken, s.index, ownerSlots);
            if (seen.contains(id)) continue;
            seen.insert(id);
            out.append({id, s.name, s.self, instanceId(salt, s.index, s.streamId),
                        s.self ? QString() : shownAddress(s.address), s.lanHttp, s.index});
        }
        return out;
    }

    QList<StreamViewer> viewers;
    /// The owner's answer to "tell me when this happens" (AppSettings
    /// stream_notifications). Travels with the snapshot so the tray client,
    /// which has no settings of its own, obeys the same switch.
    bool notify = true;

    /// False when the answer could not be obtained (the tray client's loopback
    /// call failed). NOT the same as "nobody is streaming": a tray that read an
    /// unanswered poll as an empty room would announce every viewer's departure
    /// on a hiccup, and their arrival again a moment later. Never serialized —
    /// a snapshot that reaches the wire came from the machine that knows.
    bool valid = true;

    int count() const { return viewers.size(); }

    QJsonObject toJson() const
    {
        QJsonArray arr;
        for (const StreamViewer& v : viewers)
            arr.append(QJsonObject{{QStringLiteral("id"), v.id},
                                   {QStringLiteral("name"), v.name},
                                   {QStringLiteral("self"), v.self},
                                   {QStringLiteral("instance"), v.instance},
                                   {QStringLiteral("address"), v.address},
                                   {QStringLiteral("lan_http"), v.lanHttp},
                                   {QStringLiteral("slot"), v.slot}});
        return QJsonObject{{QStringLiteral("count"), viewers.size()},
                           {QStringLiteral("viewers"), arr},
                           {QStringLiteral("notify"), notify}};
    }

    static StreamActivity fromJson(const QJsonObject& obj)
    {
        StreamActivity a;
        a.notify = obj.value(QStringLiteral("notify")).toBool(true);
        for (const QJsonValue& v : obj.value(QStringLiteral("viewers")).toArray()) {
            const QJsonObject o = v.toObject();
            a.viewers.append({o.value(QStringLiteral("id")).toString(),
                              o.value(QStringLiteral("name")).toString(),
                              o.value(QStringLiteral("self")).toBool(false),
                              o.value(QStringLiteral("instance")).toString(),
                              o.value(QStringLiteral("address")).toString(),
                              o.value(QStringLiteral("lan_http")).toBool(false),
                              o.value(QStringLiteral("slot")).toInt(-1)});
        }
        return a;
    }
};
