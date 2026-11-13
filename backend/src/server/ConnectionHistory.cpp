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

#include "ConnectionHistory.h"

#include "ViewerNotices.h"

#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSaveFile>

namespace {

const char kDateFormat[] = "yyyy-MM-dd HH:mm:ss";

QString duration(qint64 secs)
{
    secs = qMax<qint64>(0, secs);
    return QStringLiteral("%1:%2:%3")
        .arg(secs / 3600)
        .arg((secs % 3600) / 60, 2, 10, QLatin1Char('0'))
        .arg(secs % 60, 2, 10, QLatin1Char('0'));
}

QList<QByteArray> linesOf(const QByteArray& all)
{
    QList<QByteArray> out;
    for (const QByteArray& l : all.split('\n')) {
        QByteArray line = l;
        if (line.endsWith('\r')) line.chop(1);
        if (!line.isEmpty()) out.append(line);
    }
    return out;
}

} // namespace

QString ConnectionHistory::endedByName(EndedBy e)
{
    switch (e) {
    case EndedBy::Client: return QStringLiteral("client");
    case EndedBy::Tray: return QStringLiteral("tray");
    case EndedBy::Admin: return QStringLiteral("admin");
    case EndedBy::Server: return QStringLiteral("server");
    }
    return QStringLiteral("server");
}

QString ConnectionHistory::field(const QString& value)
{
    QString v = value;
    // A line break would start a line of its own in the file, and another
    // control character has no place in a name or an address.
    for (QChar& c : v)
        if (c.unicode() < 0x20 || c.unicode() == 0x7F) c = QLatin1Char(' ');
    // What a spreadsheet would run as a formula is kept as text. The test is
    // made on the value as received, before the control characters above
    // turn into spaces: a tab or a carriage return in front is a formula's
    // way in too.
    if (!value.isEmpty()) {
        const QChar first = value.at(0);
        if (first == QLatin1Char('=') || first == QLatin1Char('+') || first == QLatin1Char('-') ||
            first == QLatin1Char('@') || first == QLatin1Char('\t') || first == QLatin1Char('\r'))
            v.prepend(QLatin1Char('\''));
    }
    v.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + v + QLatin1Char('"');
}

QByteArray ConnectionHistory::headerLine()
{
    return QByteArrayLiteral(
        "\"Start\",\"End\",\"Duration\",\"Device\",\"IP\",\"Connection\",\"Ended by\"\r\n");
}

QByteArray ConnectionHistory::line(const Entry& e)
{
    const QString connection = e.plainHttp  ? QStringLiteral("Plain HTTP")
                               : e.internet ? QStringLiteral("HTTPS (Internet)")
                                            : QStringLiteral("HTTPS");
    // The name a device gave itself, cleaned as the tray cleans it: no control
    // or format character (a bidi override included), no line separator, cut
    // at ViewerNotices::kMaxNameLength, and "Someone" when nothing is left.
    StreamViewer who;
    who.name = e.device;
    const QStringList fields = {field(e.start.toString(QLatin1String(kDateFormat))),
                                field(e.end.toString(QLatin1String(kDateFormat))),
                                field(duration(e.start.secsTo(e.end))),
                                field(ViewerNotices::nameOf(who)),
                                field(e.address),
                                field(connection),
                                field(endedByName(e.endedBy))};
    return fields.join(QLatin1Char(',')).toUtf8() + "\r\n";
}

ConnectionHistory::ConnectionHistory(QString path)
    : m_Path(std::move(path))
{}

bool ConnectionHistory::append(const Entry& e)
{
    QFile file(m_Path);
    const bool fresh = !file.exists() || file.size() == 0;
    // The owner's alone, from the moment it exists: names and addresses are
    // personal data, and a file made readable to others for an instant could
    // be opened then and read for good. One older than that is set again.
    const QFileDevice::Permissions ownerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append, ownerOnly)) return false;
    file.setPermissions(ownerOnly);
    if (fresh && file.write(headerLine()) < 0) return false;
    return file.write(line(e)) >= 0;
}

QDateTime ConnectionHistory::startOf(const QByteArray& line)
{
    // "yyyy-MM-dd HH:mm:ss" quoted, then a comma: 22 bytes the line begins with.
    if (line.size() < 22 || line.at(0) != '"' || line.at(20) != '"' || line.at(21) != ',')
        return {};
    return QDateTime::fromString(QString::fromLatin1(line.mid(1, 19)), QLatin1String(kDateFormat));
}

void ConnectionHistory::purge(const QDateTime& now, qint64 maxBytes)
{
    QFile in(m_Path);
    if (!in.exists() || !in.open(QIODevice::ReadOnly)) return;
    const QList<QByteArray> all = linesOf(in.readAll());
    in.close();

    const QDateTime oldest = now.addDays(-kKeepDays);
    QList<QByteArray> kept;
    for (const QByteArray& l : all) {
        const QDateTime start = startOf(l);
        if (start.isValid() && start >= oldest) kept.append(l);
    }
    // Over the cap: the oldest lines go first.
    qint64 size = headerLine().size();
    for (const QByteArray& l : std::as_const(kept))
        size += l.size() + 2;
    while (!kept.isEmpty() && size > maxBytes) {
        size -= kept.first().size() + 2;
        kept.removeFirst();
    }
    if (kept.size() + 1 == all.size()) return; // nothing to drop: the header and every line

    QSaveFile out(m_Path);
    if (!out.open(QIODevice::WriteOnly)) return;
    out.write(headerLine());
    for (const QByteArray& l : std::as_const(kept))
        out.write(l + "\r\n");
    if (out.commit())
        QFile::setPermissions(m_Path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

QByteArray ConnectionHistory::download(const QDateTime& now) const
{
    QByteArray out = headerLine();
    QFile in(m_Path);
    if (!in.exists() || !in.open(QIODevice::ReadOnly)) return out;
    const QDateTime oldest = now.addDays(-kKeepDays);
    for (const QByteArray& l : linesOf(in.readAll())) {
        const QDateTime start = startOf(l);
        if (start.isValid() && start >= oldest) out += l + "\r\n";
    }
    return out;
}
