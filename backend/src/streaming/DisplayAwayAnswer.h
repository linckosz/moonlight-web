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
#include <QLatin1String>
#include <QString>

// The answer to a start that failed because a Mac's display was away, for
// whoever asked.
//
// The native macOS session words the reason for the Mac's owner ("the Mac's
// screen saver is on, and the Mac is locked — wake and unlock it, then start
// the stream again (…)") behind a stable tag, "mac_display_away: "
// (native-host/src/platform/macos/DisplayAway.h, which spells the tag too: the
// backend tests hold the two equal). Here the tag leaves the text for the
// answer's "code", next to "error", the way video_capture_failed or
// app_running already travel — so the owner's page shows the sentence alone,
// and a translation can later key on the code instead of parsing English.
//
// A guest of a shared stream (an invitation, or a booth by PIN) is not the
// owner: that the Mac is locked tells a stranger its owner is away, and its
// stream has not started, so it has not seen that screen either. On the way
// out to a guest (startPlayerStream in main.cpp) the reason is replaced by a
// neutral sentence under a code of its own. The full reason stays in the
// host's log.
class DisplayAwayAnswer
{
public:
    /// The tag the native session puts at the head of its error, and the code
    /// the owner's answer carries.
    static constexpr const char* kCode = "mac_display_away";
    /// What a guest's answer carries instead.
    static constexpr const char* kGuestCode = "host_screen_unavailable";
    static constexpr const char* kGuestText = "The host's screen is not available right now.";

    /// The body of a failed start's answer, as HttpResponse::error builds it
    /// ({error, status}), with the tag of a display away moved into "code".
    static QJsonObject startFailure(const QString& error, int status)
    {
        QJsonObject body;
        const QString head = QLatin1String(kCode) + QLatin1String(": ");
        if (error.startsWith(head)) {
            body[QStringLiteral("error")] = error.mid(head.size());
            body[QStringLiteral("code")] = QLatin1String(kCode);
        } else {
            body[QStringLiteral("error")] = error;
        }
        body[QStringLiteral("status")] = status;
        return body;
    }

    /// @p body as a guest may read it: a display away said neutrally, any
    /// other answer as it is. Fails closed: the tag anywhere in the text is
    /// enough, so a reason that reached here still tagged — not through
    /// startFailure (a worker body that was no object, a plain
    /// HttpResponse::error some day) — is replaced all the same.
    static void forGuest(QJsonObject& body)
    {
        const bool coded = body.value(QStringLiteral("code")).toString() == QLatin1String(kCode);
        const bool tagged =
            body.value(QStringLiteral("error")).toString().contains(QLatin1String(kCode));
        if (!coded && !tagged) return;
        body[QStringLiteral("error")] = QLatin1String(kGuestText);
        body[QStringLiteral("code")] = QLatin1String(kGuestCode);
    }
};
