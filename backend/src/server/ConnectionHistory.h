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

#include <QByteArray>
#include <QDateTime>
#include <QString>

/**
 * Who streamed this machine's screen, when, from where, over what, and how it
 * ended: one line per stream, kept for the owner (issue #42). It takes the
 * place of the tray's "Recent viewers", which lived in memory only, and is what
 * the admin page's "Download connection history" hands out.
 *
 * A security audit, so the full address is kept, a public one included, and so
 * is the device's name. That makes it personal data: the file sits in the
 * server's data folder (never in its logs folder, so never in the logs archive
 * that is made to be shared), no other ordinary account can read it, and
 * it keeps @ref kKeepDays days and @ref kMaxBytes bytes at most, the oldest
 * lines going first. No token and no session id are ever written to it.
 *
 * The file is CSV (RFC 4180): every field quoted, a quote doubled, and a field
 * that a spreadsheet would read as a formula (starting with = + - @, a tab or a
 * carriage return) prefixed with an apostrophe. Pure but for the file itself,
 * whose path and clock are the caller's (tests/test_connection_history.cpp).
 */
class ConnectionHistory
{
public:
    static constexpr int kKeepDays = 90;
    static constexpr qint64 kMaxBytes = 5LL * 1024 * 1024;
    /// The file's name, in the server's data folder.
    static constexpr const char* kFileName = "connection-history.csv";

    /// How a stream ended: a closed list, never free text. A link that went
    /// quiet is the client's: the worker that gives up on it does not say why,
    /// and a word nothing can tell apart is not written.
    enum class EndedBy
    {
        Client, ///< the viewer stopped it, left, lost its link, or took it to another device
        Tray,   ///< the tray's Disconnect
        Admin,  ///< its device was revoked, or the owner ended the guest's share
        Server  ///< the server ended it: a quit, a restart, a crashed worker
    };

    struct Entry
    {
        QDateTime start;
        QDateTime end;
        QString device;         ///< its name in the sessions list, written as the tray shows it
        QString address;        ///< the full address, public included
        bool plainHttp = false; ///< a TV on plain HTTP
        bool internet = false;  ///< reached from outside this LAN
        EndedBy endedBy = EndedBy::Client;
    };

    /// The file in the server's data folder @p dataDir: at its root, never in
    /// its logs folder, which the logs archive takes whole.
    static QString pathIn(const QString& dataDir)
    {
        return dataDir + QLatin1Char('/') + QLatin1String(kFileName);
    }

    static QString endedByName(EndedBy e);

    /// Who may download the file (POST /api/admin/connection-history): the
    /// administrator alone. @p isLocal is the admin privilege as HttpServer
    /// grants it, which on a POST takes the admin key; the local channel gets
    /// it without one, and a script of this machine may drive the channel, so
    /// the channel is refused. So is a TV on plain HTTP, which never reaches
    /// the route anyway (LanHttp's routes).
    static bool mayDownload(bool isLocal, bool localChannel, bool lanHttp)
    {
        return isLocal && !localChannel && !lanHttp;
    }

    /// One field as written: quoted, quotes doubled, a formula defused, and
    /// a line break or another control character turned into a space.
    static QString field(const QString& value);
    /// The header line, then one line per stream: "\r\n"-terminated each.
    static QByteArray headerLine();
    static QByteArray line(const Entry& e);

    explicit ConnectionHistory(QString path);

    /// Adds one stream's line at the end, the header first when the file is
    /// new. The file is made readable by its owner only.
    bool append(const Entry& e);

    /// Drops the lines older than kKeepDays at @p now, and the oldest ones
    /// while the file is over kMaxBytes. A line that cannot be read goes too.
    void purge(const QDateTime& now, qint64 maxBytes = kMaxBytes);

    /// The file as downloaded: the header and every line that reads as one,
    /// within kKeepDays at @p now. A line that cannot be read is left out and
    /// never fails the whole; a missing file is the header alone.
    QByteArray download(const QDateTime& now) const;

    /// When a line began, or an invalid date when it does not read as a line.
    static QDateTime startOf(const QByteArray& line);

private:
    QString m_Path;
};
