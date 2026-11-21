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
#include <QList>

#include <optional>

/**
 * @brief The picture an invited player gets (issue #67): its height and its
 * bitrate, worked out here from what the join page says.
 *
 * The guest picks Auto or one of three rungs; nothing else about the stream is
 * theirs. Auto takes the guest's screen: the host's picture, at the HOST's
 * shape (the stream is the host's picture, so a guest's shape would only add
 * bars inside it), laid in that screen, and the tallest rung of 720, 1080 and
 * 1440 that fits whole; on a TV, 720, as the owner's Auto. The bitrate is the
 * owner's Auto rule (frontend/js/util/AutoBitrate.js, computeAutoBitrate) on
 * the guest's network, for Auto and a rung alike. The page only describes its
 * screen: a size outside sane bounds is no screen, and no field it sends can
 * ask for more than 1440 lines or name a bitrate.
 *
 * Pure: no Qt event loop, no state. The table both sides of the rule are held
 * to is frontend/test/fixtures/guest-profile-cases.json.
 */
namespace mw::guestprofile {

/// The rungs a guest's picture is one of, lowest first.
constexpr int kRungs[] = {720, 1080, 1440};
/// The rung of a guest whose screen is unknown, and of an unknown height.
constexpr int kDefaultHeight = 1080;
/// A TV's Auto, as the owner's (StreamResolution.js TV_AUTO_MAX_HEIGHT).
constexpr int kTvHeight = 720;
/// A screen side the page may describe, in device pixels: anything outside is
/// taken for no screen at all.
constexpr int kScreenMin = 320;
constexpr int kScreenMax = 8192;

/// A guest's screen in device pixels, as the join page measured it.
struct Screen
{
    int width = 0;
    int height = 0;
};

/// The screen a join request describes ("screen": "WxH"), or none when it is
/// absent, malformed or out of [kScreenMin, kScreenMax] on either side.
std::optional<Screen> parseScreen(const QJsonObject& body);

/// Auto's height for a guest: the tallest rung whose picture, at @p hostRatio
/// (width / height; 16:9 when not positive), fits @p screen whole; 720 when
/// none does; kTvHeight on a TV; kDefaultHeight with no screen.
int autoHeight(const std::optional<Screen>& screen, double hostRatio, bool tv);

/// The rung a join request names: 0 for Auto ("auto": true), whose height
/// autoHeight works out once the host's shape is known; else its "height"
/// when it is a rung, else kDefaultHeight.
int requestedRung(const QJsonObject& body);

/// Whether a join request says its screen is a TV's ("tv": true).
bool requestFromTv(const QJsonObject& body);

/// The owner's Auto bitrate (AutoBitrate.js computeAutoBitrate, SDR), in kbps:
/// the LAN rule when @p lan, the Internet one otherwise.
int autoBitrateKbps(int height, double aspectRatio, bool lan);

/// Auto's height held to the host's own screen (G3 of issue #67's second
/// step): the tallest rung no taller than @p hostScreenHeight, 720 at the
/// least. Unchanged when the host's screen is unknown (0 or less).
int capToHost(int height, int hostScreenHeight);

/// The host's screen capToHost holds Auto to: the height of the display the
/// stream captures, @p capturedHeight, where the host says which it is (this
/// machine's native host, from its probe); else the tallest of the host's
/// modes, @p modeHeights (a GameStream host's serverinfo); 0 when neither is
/// known. The native host lists no modes at all, and its tallest display is
/// not the one shown: a 1080p screen captured beside a 1440p virtual display
/// streams 1080 lines, never 1440 (GUESTD1, 21/11/2026).
int shownScreenHeight(int capturedHeight, const QList<int>& modeHeights);

// ── The guest's congestion ladder (issue #67, step 2) ──────────────────────
//
// A guest of a GameStream host (Sunshine, Wolf, Apollo) has the owner's ladder
// for a stream that keeps choking: its page relaunches its own leg one step
// lower ("degrade": n), and one step higher again once the link has been quiet
// a while. The page names a step, never a height or a bitrate: step n is
// worked out here from the picture the guest would have at step 0, and only
// ever lowers it. A native host needs none of it: its rate governor follows
// the guest's link between two frames.

/// The lowest step: past it, nothing is left to lower.
constexpr int kDegradeMax = 8;
/// Where the ladder stops lowering each knob.
constexpr int kDegradeFloorHeight = 720;
constexpr int kDegradeFloorKbps = 2000;

/// The step a join request asks for. Only a JSON integer counts: 0 to
/// kDegradeMax as it is, kDegradeMax above it; a string, a fraction, a
/// negative number, anything else, 0.
int readDegrade(const QJsonObject& body);

/// Whether a join request is the guest's own ladder relaunching its leg: it
/// carries a "degrade" readDegrade reads as a step (a JSON integer, 0 or
/// more). Any other "degrade" makes it an ordinary join.
bool asksLadderStep(const QJsonObject& body);

/// A guest's picture: its height and its bitrate.
struct Picture
{
    int height = kDefaultHeight;
    int bitrateKbps = 0;
    bool operator==(const Picture& o) const
    {
        return height == o.height && bitrateKbps == o.bitrateKbps;
    }
};

/// The picture at step @p n (readDegrade's bounds) from @p base, the guest's
/// at step 0, at @p aspectRatio on the guest's network (@p lan). As the
/// owner's ladder (app.js _onStreamCongested), one step at a time: the bitrate
/// halved at the first, cut by 30 % at each next one, never under
/// kDegradeFloorKbps; the height one rung down at each while above 720, the
/// bitrate then no more than Auto's for the new height. Never above @p base.
Picture degraded(const Picture& base, double aspectRatio, bool lan, int n);

// ── Auto on the native host's shared feed (issue #67, step 2) ──────────────
//
// One picture for every guest: in Auto, the tallest of the guests' own Auto
// heights (I67-flux A), so the largest screen sees it sharp and the others
// scale it down. Only a guest whose browser is connected counts (G1); the
// feed never goes down while guests watch it (I67-baisse B: one cut fewer for
// everyone), never above the host's screen nor 1440 (G3); and it is rebuilt
// up at most once a minute, all guests together (G2, SharedFeed).

/// Between two rebuilds of the feed in Auto (G2).
constexpr qint64 kFeedAutoRebuildGapMs = 60000;

/// A guest on the feed, as Auto weighs it.
struct FeedGuest
{
    int wantHeight = kDefaultHeight; ///< its own Auto height (autoHeight, capToHost)
    bool lan = true;                 ///< on this machine's network
    bool connected = false;          ///< its browser has the stream (G1)
};

/// What the feed in Auto should be.
struct FeedTarget
{
    int height = kDefaultHeight;
    /// Every connected guest on the LAN: the LAN rule's bitrate, else the
    /// Internet one (autoBitrateKbps).
    bool lan = true;
    /// The guests that counted: the connected ones.
    int counted = 0;
};

/// The feed's Auto target from @p current (its height and rule now) and its
/// @p guests. With @p mayLower false — the feed already in Auto — it never
/// goes below current.height, nor from the Internet rule back to the LAN one;
/// true — the owner has just picked Auto — it is the connected guests' alone.
/// No connected guest: current, as it is.
FeedTarget feedAutoTarget(const FeedTarget& current, const QList<FeedGuest>& guests,
                          int hostScreenHeight, bool mayLower);

} // namespace mw::guestprofile
