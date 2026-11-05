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

#include <QJsonObject>
#include <QString>

/**
 * The session cookie handed back to the browser, by every route that opens a
 * session (routes/AuthRoutes.cpp, routes/PairRequestRoutes.cpp,
 * routes/PinProofRoutes.cpp).
 *
 * Remembered sessions carry a 90-day Max-Age, matching the server-side sliding
 * TTL. A session the visitor asked not to remember deliberately carries none:
 * a cookie without Max-Age/Expires is session-scoped and dies with the browser
 * process, which is what someone on a public machine expects. Never add one
 * later either — the periodic refresh in /api/auth/status would otherwise
 * promote a temporary login to 90 days behind the user's back.
 */
inline QString sessionCookie(const QString& token, bool remember)
{
    QString cookie =
        QStringLiteral("mw_session=%1; HttpOnly; Secure; Path=/; SameSite=Strict").arg(token);
    if (remember) cookie += QStringLiteral("; Max-Age=7776000");
    return cookie;
}

/// Expire the session cookie immediately (logout).
inline QString clearedSessionCookie()
{
    return QStringLiteral("mw_session=; HttpOnly; Secure; Path=/; SameSite=Strict; Max-Age=0");
}

/// "remember me" is opt-out: a body without the field keeps the old 90-day
/// behaviour, so older clients and the share flow are unaffected.
inline bool wantsRemember(const QJsonObject& body)
{
    return body.value(QStringLiteral("remember")).toBool(true);
}
