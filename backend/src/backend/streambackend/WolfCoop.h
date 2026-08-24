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

#include "../WolfApiClient.h"

/// The one decision co-op session reaping turns on, kept apart from the
/// transport so it can be reasoned about — and tested — on its own. It encodes a
/// fact about Wolf that is invisible from our side (a session is identified by
/// the launch key it was created with), which is exactly why it does not belong
/// inline in a network callback.
namespace WolfCoop {

/// Which of Wolf's live sessions is the one OUR launch created.
///
/// Wolf stores the `rikey` query parameter of the launch verbatim as the
/// session's `aes_key` (state/sessions.hpp: create_stream_session), and we send
/// that key hex-encoded (NvHTTP). So this is a hex comparison — case-insensitive
/// because case carries no meaning in hex and a host that normalised it must
/// still match.
///
/// It is the only collision-free key available: two of our own concurrent
/// sessions on one host share `client_ip`, resolution and app id, and the
/// session id itself is what we are trying to learn.
///
/// An empty result is a legitimate answer — the session may already have ended —
/// and must never be softened into "the first session, probably ours".
inline QString matchSessionByLaunchKey(const QVector<WolfStreamSession>& sessions,
                                       const QByteArray& launchKey)
{
    if (launchKey.isEmpty()) return {};
    const QString wanted = QString::fromLatin1(launchKey.toHex());
    for (const WolfStreamSession& s : sessions) {
        if (s.aesKey.compare(wanted, Qt::CaseInsensitive) == 0) return s.sessionId;
    }
    return {};
}

} // namespace WolfCoop
