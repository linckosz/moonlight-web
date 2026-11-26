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

#include "common/Logger.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QString>

/// One line of moonlightweb-client.log, with its level.
struct ClientLogLine
{
    Logger::Level level;
    QString text;
};

/// What moonlightweb-client.log gets of one batch of a page's console
/// (routes/LogRoutes.cpp): its lines in order, each tagged with the page's id,
/// @p who sent it and from @p address — both the server's, never the page's.
/// The first batch (seq 0) opens with the page's address and user agent.
///
/// What a page wrote is data, never a line of ours: each line is cut to
/// kClientLineChars, and its line breaks and control characters are made
/// visible rather than kept (neutralizeClientText), so no line can pass for one
/// the server wrote. Share links are written /p/(link), and the value of a
/// token= or a JSON "token" (hidden) (LogScrubber::hideShareLinks); a page's
/// secrets are hidden too (hideClientSecrets).
QList<ClientLogLine> clientLogLines(const QJsonObject& body, const QString& who,
                                    const QString& address);

/// Whether the client log takes a batch at all (routes/LogRoutes.cpp): in debug
/// mode, from anyone it is open to; out of it, only where @p outOfDebug — an
/// owner's page, for its warnings and errors. A guest's page is answered 404
/// there, as before.
bool clientLogHeard(bool debug, bool outOfDebug);

/// The longest line a page may write, past which it is cut and marked " […]".
constexpr qsizetype kClientLineChars = 2 * 1024;

/// @p text on one line: a line break becomes " ⏎ ", any other control character
/// (C0, DEL, C1, and the Unicode line and paragraph separators) U+FFFD.
QString neutralizeClientText(QString text);

/// @p text without what a page's URLs and requests may carry that is no one
/// else's business: the value of a link fragment's k= and n= (the owner link's
/// keys), of a code=, pin=, key=, token=, password=, session= or auth= parameter,
/// of a Cookie, Set-Cookie or Authorization header, and of an mw_ cookie, each
/// written (hidden). The page hides the same before it sends (ClientLog.js); the
/// server does it again, because a page may not.
QString hideClientSecrets(QString text);

/// Who a session's batch is counted against (ClientLogBudget): its session,
/// by a digest of the cookie's token, or the address of a caller that has
/// none (the host machine itself). Never the token itself.
QString clientLogSender(const QString& sessionToken, const QString& address);

/**
 * How much of a page's console the client log takes, per sender and per
 * minute: lines and bytes, in debug mode as out of it. A page can mark anything
 * as an error, so the level alone bounds nothing. What goes past the budget is
 * dropped and counted, and the count is written once, when the sender's next
 * minute begins ("N client lines dropped"). The file's own size stays bounded
 * by the logger's rotation (Logger::kMaxFileBytes, kMaxTotalBytes).
 *
 * A sender is what the server knows of the caller: its session, a guest's
 * slot, the host machine's address — never anything the page says. Thread-safe.
 */
class ClientLogBudget
{
public:
    struct Limits
    {
        int lines;
        qint64 bytes;
    };
    /// Debug mode: the whole console of a busy page, its [perf] line a second
    /// and a stream's start included, with room to spare.
    static constexpr Limits kDebug{3000, 1024 * 1024};
    /// Out of it: warnings and errors only. A healthy page writes a handful.
    static constexpr Limits kQuiet{120, 64 * 1024};
    static constexpr qint64 kWindowMs = 60 * 1000;
    /// Senders remembered at once; the stalest goes first past this.
    static constexpr int kMaxSenders = 256;

    /// What of @p lines @p sender may write at @p nowMs, in order, preceded by
    /// the count of what its previous minute dropped, if anything was.
    QList<ClientLogLine> admit(const QString& sender, const QList<ClientLogLine>& lines,
                               const Limits& limits, qint64 nowMs);

private:
    struct Window
    {
        qint64 start = 0;
        int lines = 0;
        qint64 bytes = 0;
        int dropped = 0;
    };
    QMutex m_Mutex;
    QHash<QString, Window> m_Windows;
};
