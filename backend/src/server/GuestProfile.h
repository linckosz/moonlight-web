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

} // namespace mw::guestprofile
