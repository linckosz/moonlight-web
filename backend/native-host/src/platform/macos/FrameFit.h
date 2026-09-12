/*
 * MoonlightWeb — native capture & encoding engine.
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

#include <algorithm>
#include <cmath>

// Where the desktop lands inside the frame, on a macOS host.
//
// ScreenCaptureKit scales the desktop into the size the session asked for while
// KEEPING its aspect (scalesToFit), so a panel shaped unlike the stream is
// letterboxed: the 1.54:1 panel of a 14" MacBook Pro captured at 1920x1080
// becomes a 1663-pixel-wide picture centred between two 129-pixel bars. Nothing
// else in the pipeline knows: the client is handed 1920x1080 and aims at all of
// it, and the input sink stretches all of it back over the display.
//
// Every conversion between a desktop position and a frame position therefore
// has to go through the same fit, or it is off by up to the width of a bar.
// Both failures were live on 12/09/2026, and both are quiet:
//
//  - the magnified pointer the engine draws for a phone was placed by scaling
//    the display's width onto the FRAME's width, so it drifted outwards and,
//    near either edge, was drawn ON a bar — where the picture never repaints
//    over it, so every position it was drawn at stayed visible at once;
//  - a tap was mapped from the whole frame onto the whole display, so it landed
//    short of what it was aimed at by up to 7% of the width.
//
// Pure arithmetic, no platform: tested on every OS, like CursorBlend.h.

namespace mw::native::platform {

/// The placement of a src-sized picture inside a dst-sized frame, aspect kept.
struct FrameFit
{
    float scale = 1.0f;   ///< source units → frame pixels
    float offsetX = 0.0f; ///< the bar on the left, in frame pixels
    float offsetY = 0.0f; ///< the bar on top, in frame pixels
};

/// The fit ScreenCaptureKit applies. A degenerate size gives the identity,
/// which is what every caller wants before the session knows its own geometry.
inline FrameFit frameFit(double srcW, double srcH, double dstW, double dstH)
{
    FrameFit fit;
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return fit;
    const double scale = std::min(dstW / srcW, dstH / srcH);
    fit.scale = static_cast<float>(scale);
    fit.offsetX = static_cast<float>((dstW - srcW * scale) / 2.0);
    fit.offsetY = static_cast<float>((dstH - srcH * scale) / 2.0);
    return fit;
}

/// Take the letterbox back out of a point the client aimed at the frame.
///
/// The answer is in the SAME frame coordinates, as if the desktop filled the
/// whole frame — which is exactly what an input sink that stretches the client's
/// reference over the display assumes. The bars are no part of the desktop, so a
/// point on one clamps to the edge it is nearest.
///
/// Returns false when there is nothing to map (an empty reference, or a picture
/// the fit leaves no room for), and leaves the point alone.
inline bool unletterboxPoint(const FrameFit& fit, int refW, int refH, double x, double y, int& outX,
                             int& outY)
{
    if (refW <= 0 || refH <= 0) return false;
    const double pictureW = refW - 2.0 * fit.offsetX;
    const double pictureH = refH - 2.0 * fit.offsetY;
    if (pictureW <= 0.0 || pictureH <= 0.0) return false;
    const double aimedX = (x - fit.offsetX) * refW / pictureW;
    const double aimedY = (y - fit.offsetY) * refH / pictureH;
    outX = static_cast<int>(std::lround(std::clamp(aimedX, 0.0, static_cast<double>(refW - 1))));
    outY = static_cast<int>(std::lround(std::clamp(aimedY, 0.0, static_cast<double>(refH - 1))));
    return true;
}

} // namespace mw::native::platform
