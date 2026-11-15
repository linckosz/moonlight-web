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

#include "GuestProfile.h"

#include <QRegularExpression>
#include <QString>

#include <algorithm>
#include <cmath>

namespace mw::guestprofile {

namespace {

// AutoBitrate.js: the Internet rule (9.5 Mbps at 1080p, pixels^0.75) and the
// LAN one (20 Mbps at 1080p, pixels^0.8). Change them there and here together:
// the shared table (guest-profile-cases.json) fails on whichever side is left.
constexpr double kAutoMbps1080p = 9.5;
constexpr double kAutoPixelExponent = 0.75;
constexpr double kAutoLanMbps1080p = 20.0;
constexpr double kAutoLanPixelExponent = 0.8;

double ratioOr169(double ratio)
{
    return ratio > 0.0 && std::isfinite(ratio) ? ratio : 16.0 / 9.0;
}

/// The stream's width for @p height lines at @p ratio, as the guest's launch
/// works it out (even, rounded).
int widthFor(int height, double ratio)
{
    return static_cast<int>(height * ratio + 0.5) & ~1;
}

/// AutoBitrate.js roundUpMbps: up to a whole Mbps to 30, to 5 Mbps above.
double roundUpMbps(double mbps)
{
    const double x = mbps - 1e-9; // a value already whole stays itself
    return x > 30.0 ? std::ceil(x / 5.0) * 5.0 : std::ceil(x);
}

} // namespace

std::optional<Screen> parseScreen(const QJsonObject& body)
{
    static const QRegularExpression kSize(QStringLiteral("^(\\d{1,5})x(\\d{1,5})$"));
    const QJsonValue value = body.value(QStringLiteral("screen"));
    if (!value.isString()) return std::nullopt;
    const QRegularExpressionMatch m = kSize.match(value.toString());
    if (!m.hasMatch()) return std::nullopt;
    const int width = m.captured(1).toInt();
    const int height = m.captured(2).toInt();
    const auto inBounds = [](int side) { return side >= kScreenMin && side <= kScreenMax; };
    if (!inBounds(width) || !inBounds(height)) return std::nullopt;
    return Screen{width, height};
}

int autoHeight(const std::optional<Screen>& screen, double hostRatio, bool tv)
{
    if (tv) return kTvHeight;
    if (!screen) return kDefaultHeight;
    const double ratio = ratioOr169(hostRatio);
    // Whichever way it is held: a phone measured upright is turned to watch a
    // landscape picture, so the picture is laid along the screen's long side
    // (its short side, for a host taller than wide).
    const int longSide = std::max(screen->width, screen->height);
    const int shortSide = std::min(screen->width, screen->height);
    const int across = ratio >= 1.0 ? longSide : shortSide;
    const int down = ratio >= 1.0 ? shortSide : longSide;
    int best = kRungs[0];
    for (const int rung : kRungs)
        if (rung <= down && widthFor(rung, ratio) <= across) best = rung;
    return best;
}

int requestedRung(const QJsonObject& body)
{
    if (body.value(QStringLiteral("auto")).toBool(false)) return 0;
    const int height = body.value(QStringLiteral("height")).toInt(kDefaultHeight);
    for (const int rung : kRungs)
        if (height == rung) return rung;
    return kDefaultHeight;
}

bool requestFromTv(const QJsonObject& body)
{
    return body.value(QStringLiteral("tv")).toBool(false);
}

int autoBitrateKbps(int height, double aspectRatio, bool lan)
{
    const double h = height > 0 ? height : 1080.0;
    // Pixel count normalised to 1920×1080, as computeAutoBitrate counts it.
    const double pixelRatio = (h * h * ratioOr169(aspectRatio)) / (1920.0 * 1080.0);
    const double mbps = lan ? kAutoLanMbps1080p * std::pow(pixelRatio, kAutoLanPixelExponent)
                            : kAutoMbps1080p * std::pow(pixelRatio, kAutoPixelExponent);
    const double rounded = std::clamp(roundUpMbps(mbps), 1.0, 150.0);
    return static_cast<int>(rounded * 1000.0 + 0.5);
}

} // namespace mw::guestprofile
