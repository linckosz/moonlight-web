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

#include "common/Types.h"
#include "server/ShareManager.h"

#include <functional>
#include <utility>

class HttpServer;

/**
 * Everything the share routes need from the stream lifecycle, which lives in
 * main.cpp. Passed in rather than reached for, so ShareRoutes stays a thin
 * HTTP layer over ShareManager.
 */
struct ShareRoutesDeps
{
    /// Start a player's stream on their slot. Answers with the same payload as
    /// POST /api/hosts/:id/start so the frontend reuses its pipeline verbatim.
    /// @p height is one of 720/1080/1440; @p aspect is the player's "W:H" screen
    /// ratio (width is derived from it) — everything else (fps, codec, bitrate)
    /// is decided server-side. @p serverHost is the hostname the player reached
    /// us on, needed to build the signaling URL they must come back to.
    std::function<void(int slot, int height, QString aspect, ShareManager::Permissions perms,
                       QString serverHost, ResponseCallback respond)>
        startPlayerStream;

    /// Tear a player's worker down without touching Sunshine or other slots.
    /// @p notifyEnded sends the "the owner ended it" notice first — true when
    /// the owner pulled the plug, false when the player left on their own.
    std::function<void(int slot, bool notifyEnded)> stopPlayerStream;

    /// True when there is a host app running that a player could resume into.
    /// Sunshine refuses /resume with "No running app to resume" otherwise, so
    /// this is checked before the join instead of failing mid-handshake.
    std::function<bool()> ownerStreamAlive;

    /// The host uuid + app id the owner is streaming *right now*, empty uuid when
    /// nothing is up. Read at activation time to bind the share to that host, so
    /// the link can never later be routed to a different machine.
    std::function<std::pair<QString, int>()> currentOwnerContext;

    /// The host machine's display name, shown on the "Join <machine>" button.
    std::function<QString()> machineName;

    /// Origin a player can reach us on ("https://host[:port]"), or an empty
    /// string when only the LAN address will do — the caller then falls back to
    /// this machine's LAN IPv4. Never loopback: the player is on another PC.
    std::function<QString()> publicOrigin;
};

/// Register the share API. A no-op when ShareManager::kSessionSharingEnabled is
/// false, which is exactly what makes the feature vanish (unknown route → 404).
void registerShareRoutes(HttpServer& server, ShareManager& share, const ShareRoutesDeps& deps);
