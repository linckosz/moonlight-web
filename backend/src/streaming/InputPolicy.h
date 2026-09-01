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

// What a streaming session's client is allowed to send to the host.
//
// The owner's own sessions run unrestricted. A session shared with an invited
// player carries whatever the owner ticked when they created the share link,
// and the policy is fixed for the life of the worker — the popin freezes it
// before the link is ever usable.
//
// Enforced in the worker rather than in the page, because the page belongs to
// someone else and can send whatever JSON it likes. Deliberately dependency-free
// so both the relays and the codec helpers can hold one.

#include <QString>

namespace InputMsg {

struct Policy
{
    bool gamepad = true;
    bool keyboardMouse = true;

    bool unrestricted() const { return gamepad && keyboardMouse; }
};

/// Whether a message of this type may act on the host under @p p.
///
/// Refused messages are dropped in silence — answering "forbidden" would tell a
/// probing client exactly which permissions it has. Channel housekeeping (ping,
/// keyframe requests) is never input and always passes, and so does
/// 'framefloor': it says how often a still picture should be re-sent to the
/// player watching, which is about their own view and not about acting on the
/// host — a view-only guest reading a document is precisely who needs it. The
/// 'inputstate' heartbeat carries both worlds, so it passes here and is
/// filtered field by field (see InputMessageCodec.h's filterHeldState).
inline bool allowed(const QString& type, const Policy& p)
{
    if (type.startsWith(QLatin1String("gamepad"))) return p.gamepad;
    if (type == QLatin1String("ping") || type == QLatin1String("requestidr") ||
        type == QLatin1String("request_idr") || type == QLatin1String("clientstats") ||
        type == QLatin1String("framefloor"))
        return true;
    if (type == QLatin1String("inputstate")) return true;
    return p.keyboardMouse; // key*, mouse*, wheel, textinput, locksync, clipboard
}

} // namespace InputMsg
