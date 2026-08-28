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

    /// What joining @p slot would do right now: the display name of the app the
    /// invitation leads to, and whether the guest's arrival would *launch* it
    /// rather than join something already up. Answered per slot, because an
    /// invitation is bound to one host and what runs elsewhere is beside the
    /// point. An empty name means the app could not be resolved (host not polled
    /// yet); the join page then stays silent about it rather than guessing.
    std::function<std::pair<QString, bool>(int slot)> joinTarget;

    /// The host uuid + app id the owner is streaming *right now*, empty uuid when
    /// nothing is up. Read at activation time to bind the share to that host, so
    /// the link can never later be routed to a different machine.
    std::function<std::pair<QString, int>()> currentOwnerContext;

    /// Whether @p hostUuid is a known, paired host and @p appId one of its apps.
    /// Checked when an invitation is opened cold — with no stream running, the
    /// host and app come from the board rather than from what is already up, so
    /// they are input and get treated as such.
    std::function<bool(const QString& hostUuid, int appId)> hostAppExists;

    /// The host machine's display name, shown on the "Join <machine>" button.
    std::function<QString()> machineName;

    /// The whole address a guest opens for @p token, or an empty string when
    /// this machine has no way of being reached from off its own network — the
    /// caller then falls back to its LAN IPv4. Never loopback: the guest is on
    /// another PC.
    ///
    /// A whole URL rather than an origin, because the form it takes is not one.
    /// The path is spent on this machine's rendezvous identifier, which the
    /// introduction server has to see to route the call at all, and the
    /// invitation rides in the fragment, which a browser never sends to the
    /// server it fetched the page from. Composing it here from an origin would
    /// put the token in that server's request line.
    std::function<QString(const QString& token)> playerLink;

    /// Whether this machine is reachable from outside RIGHT NOW.
    ///
    /// Deliberately not part of the decision above: an invitation is made to be
    /// opened later, so a link stays valid — and stays the same characters —
    /// through a line that comes and goes. This only lets the board say so,
    /// rather than letting the owner hand out an address believing it answers.
    std::function<bool()> remoteReachable;

    /// True when this machine currently reports anonymous session statistics.
    /// A guest's own session is one of the things counted, so the join page
    /// says so — they cannot change the answer, but they get to know it.
    std::function<bool()> statsReporting;
};

/// Register the share API. A no-op when ShareManager::kSessionSharingEnabled is
/// false, which is exactly what makes the feature vanish (unknown route → 404).
void registerShareRoutes(HttpServer& server, ShareManager& share, const ShareRoutesDeps& deps);
