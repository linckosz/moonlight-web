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

#include "server/ClientLogLines.h"
#include "server/LogScrubber.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>

namespace {

constexpr qsizetype kMaxLineChars = 8 * 1024;

Logger::Level levelOf(const QString& l)
{
    if (l == QLatin1String("error")) return Logger::Error;
    if (l == QLatin1String("warn")) return Logger::Warning;
    if (l == QLatin1String("debug")) return Logger::Debug;
    return Logger::Info;
}

} // namespace

QList<ClientLogLine> clientLogLines(const QJsonObject& body, const QString& who,
                                    const QString& address)
{
    QList<ClientLogLine> out;

    // Who, in every line: the page's own id (two tabs are two pages), the kind
    // of user, and where from.
    static const QRegularExpression notId(QStringLiteral("[^A-Za-z0-9]"));
    QString id = body.value(QStringLiteral("id")).toString().left(16);
    id.remove(notId);
    const QString tag = QStringLiteral("[%1 %2@%3]").arg(id, who, address);

    // Share links are hidden before anything is cut to length, so that no
    // part of one is left at a cut.
    if (body.value(QStringLiteral("seq")).toInt() == 0) {
        const QString page =
            LogScrubber::hideShareLinks(body.value(QStringLiteral("page")).toString()).left(200);
        out.append({Logger::Info,
                    tag + QStringLiteral(" page %1 — %2")
                              .arg(page, body.value(QStringLiteral("ua")).toString().left(300))});
    }
    const int dropped = body.value(QStringLiteral("dropped")).toInt();
    if (dropped > 0)
        out.append({Logger::Warning,
                    tag + QStringLiteral(" %1 earlier lines dropped (the page's buffer was full)")
                              .arg(dropped)});

    for (const QJsonValue& v : body.value(QStringLiteral("lines")).toArray()) {
        const QJsonObject line = v.toObject();
        // The page's own clock: the server's stamp says when the batch came in,
        // this one when the line was written.
        const QString when =
            QDateTime::fromMSecsSinceEpoch(qint64(line.value(QStringLiteral("t")).toDouble()))
                .toString(QStringLiteral("hh:mm:ss.zzz"));
        QString text = LogScrubber::hideShareLinks(line.value(QStringLiteral("m")).toString());
        if (text.size() > kMaxLineChars) text = text.left(kMaxLineChars) + QStringLiteral(" […]");
        text.replace(QLatin1Char('\n'), QStringLiteral("\n    "));
        out.append({levelOf(line.value(QStringLiteral("l")).toString()),
                    tag + QLatin1Char(' ') + when + QLatin1Char(' ') + text});
    }
    return out;
}
