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

#include "server/AppSettings.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

/// What POST /api/admin/settings did with the keys of issue #60.
enum class RememberMeWrite
{
    None,    ///< the body names neither key
    Written, ///< the warning is acknowledged for the value in force
    Refused, ///< 400, @p error says why, and nothing was written
};

/**
 * The keys of "how long a remembered device stays signed in" in a body sent to
 * POST /api/admin/settings, checked before the route writes anything else.
 *
 * - "remember_me_days" itself is refused: the lifetime is settings.json's
 *   alone, never a page's, nor that of whoever holds an admin session.
 * - "remember_me_days_acknowledged" is taken only when it is @p inForce, the
 *   value AuthManager applies: a stale page cannot acknowledge a value it never
 *   showed. It is then written to settings.json (atomically, the other keys
 *   kept, AppSettings::writeAll) and echoed in @p reply.
 */
inline RememberMeWrite writeRememberMe(const QJsonObject& body, int inForce, AppSettings& settings,
                                       QJsonObject& reply, QString* error)
{
    if (body.contains(QStringLiteral("remember_me_days"))) {
        if (error) *error = QStringLiteral("remember_me_days is set in settings.json only");
        return RememberMeWrite::Refused;
    }
    if (!body.contains(QStringLiteral("remember_me_days_acknowledged")))
        return RememberMeWrite::None;
    const QJsonValue ack = body.value(QStringLiteral("remember_me_days_acknowledged"));
    if (!ack.isDouble() || ack.toDouble() != static_cast<double>(inForce)) {
        if (error)
            *error = QStringLiteral("remember_me_days_acknowledged must be the value in force");
        return RememberMeWrite::Refused;
    }
    settings.setRememberMeDaysAcknowledged(inForce);
    reply[QStringLiteral("remember_me_days_acknowledged")] = inForce;
    return RememberMeWrite::Written;
}
