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

// What a take-over notice may say beyond "taken over", in a booth whose
// streams have PINs (booth_stream_pin, issue #68): how the stream was let go
// — with its PIN, or because nobody used it for the idle time — and the
// machine that took it, so its viewer reads why it ended. Nothing else, ever:
// never the PIN, never a session. Built by the server, carried by the worker's
// command line, read again there through clean() before it reaches the page.

#include <QChar>
#include <QJsonObject>
#include <QString>

namespace TakeoverNotice {

/// The longest machine name a notice carries.
constexpr int kMaxNameLength = 64;
/// The idle time a notice may name, as booth_takeover_idle_minutes allows it.
constexpr int kMaxIdleMinutes = 240;

/// A machine name as a notice may carry it: no control characters, trimmed,
/// at most kMaxNameLength characters.
inline QString cleanName(const QString& name)
{
    QString out;
    out.reserve(qMin<qsizetype>(name.size(), kMaxNameLength));
    for (const QChar c : name)
        if (!c.isNull() && c.category() != QChar::Other_Control &&
            c.category() != QChar::Other_Format)
            out.append(c);
    return out.trimmed().left(kMaxNameLength);
}

/// @p raw reduced to what a notice may carry: "reason" ("pin" or "idle"),
/// "by" (a machine name, cleaned), and for "idle" "idle_minutes". Empty when
/// it names no reason it knows: the plain notice then, as without a booth.
inline QJsonObject clean(const QJsonObject& raw)
{
    const QString reason = raw.value(QStringLiteral("reason")).toString();
    if (reason != QLatin1String("pin") && reason != QLatin1String("idle")) return {};
    QJsonObject out{{QStringLiteral("reason"), reason}};
    const QString by = cleanName(raw.value(QStringLiteral("by")).toString());
    if (!by.isEmpty()) out.insert(QStringLiteral("by"), by);
    if (reason == QLatin1String("idle")) {
        const int minutes = raw.value(QStringLiteral("idle_minutes")).toInt(0);
        if (minutes > 0 && minutes <= kMaxIdleMinutes)
            out.insert(QStringLiteral("idle_minutes"), minutes);
    }
    return out;
}

/// A stream taken over with its PIN by @p by.
inline QJsonObject withPin(const QString& by)
{
    return clean({{QStringLiteral("reason"), QStringLiteral("pin")}, {QStringLiteral("by"), by}});
}

/// A stream taken over by @p by after nobody used it for @p idleMinutes.
inline QJsonObject afterIdle(const QString& by, int idleMinutes)
{
    return clean({{QStringLiteral("reason"), QStringLiteral("idle")},
                  {QStringLiteral("by"), by},
                  {QStringLiteral("idle_minutes"), idleMinutes}});
}

} // namespace TakeoverNotice
