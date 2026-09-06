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

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
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
    /// "someone else joined" from "the same person is still here". The browser's
    /// own uniqueid, which is exactly that.
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
};

struct StreamActivity
{
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
                                   {QStringLiteral("self"), v.self}});
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
                              o.value(QStringLiteral("self")).toBool(false)});
        }
        return a;
    }
};
