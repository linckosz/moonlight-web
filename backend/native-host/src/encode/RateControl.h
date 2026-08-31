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

#include <cstdint>

namespace mw::native::encode {

/// How many bits one frame may occupy — the VBV size, shared by every encoder.
///
/// ── What this actually bounds ───────────────────────────────────────────────
///
/// A VBV of B bits at rate R means a frame is at most B bits, so it occupies
/// B/R seconds of the link. Setting B = R/fps therefore says "a frame must
/// transmit within one frame interval", which is the right bound while frames
/// really do arrive at fps.
///
/// ── Why there is a floor, and why it is 60 ──────────────────────────────────
///
/// Two things break that reasoning at high refresh rates.
///
/// First, a desktop does not produce frames at the panel's refresh rate. Desktop
/// Duplication delivers on damage, so a page of text produces almost nothing —
/// yet a 165 Hz panel made every frame budget 1/165th of the bitrate, keyframe
/// included. 55 Mbps became 41 KB for a full 1080p intra frame, which is a
/// heavily quantized, visibly soft picture. On a moving screen the next frame
/// refines it and nobody notices. On a still one nothing ever follows, so the
/// soft first frame IS the picture, for as long as the user looks at it.
///
/// Second, the tightening buys nothing. 1/60 s of link occupancy is already
/// below the frame interval of every client display in use, so squeezing a frame
/// from 16.7 ms of link time down to 6 ms removes quality without removing any
/// delay a viewer could perceive.
///
/// So: one frame's worth at the stream's rate, never less than one sixtieth of a
/// second's worth. Above 60 the budget stops shrinking; at or below 60 the
/// original per-frame bound is unchanged, because there the content really can
/// arrive that fast and the bound is doing real work.
///
/// The proper fix is a VBV that follows the rate frames are ACTUALLY produced at
/// rather than the rate they might be — that is the client-driven rate control
/// of §9.3, and it needs the client feedback loop first. This floor is the part
/// that is correct on its own.
constexpr int kVbvFrameRateFloor = 60;

inline uint32_t vbvBitsPerFrame(uint32_t bitsPerSecond, int fps)
{
    const int rate = (fps > kVbvFrameRateFloor) ? kVbvFrameRateFloor : (fps > 0 ? fps : 60);
    return bitsPerSecond / static_cast<uint32_t>(rate);
}

/// The budget a frame gets while the screen is not moving, as a multiple of the
/// stream's own.
///
/// ── Why the floor above was not enough ──────────────────────────────────────
///
/// The floor lifted the first 1080p keyframe from 41 KB to 112 KB. The cap it
/// was measured against is 55 Mbps / 60 / 8 = 114 KB — so the keyframe came out
/// pinned to the new ceiling exactly as it had been pinned to the old one. The
/// encoder is not choosing 112 KB, it is being cut off there, and a still 1080p
/// desktop needs three to five times that to render text without softening it.
///
/// ── Why raising it is free, here and only here ──────────────────────────────
///
/// A VBV bounds how long one frame occupies the link. That bound protects the
/// NEXT frame — and while the screen is still, there is no next frame. The link
/// is carrying nothing, so a frame that takes several frame intervals to
/// transmit delays nothing at all. The moment something moves, the ordinary
/// budget is restored before that frame is encoded, so motion never pays for it.
///
/// ── Why it is expressed as a bitrate ────────────────────────────────────────
///
/// Constant bitrate aims each frame at rate/fps bits and the VBV caps it. Both
/// have to rise together: a bigger cap alone changes nothing when the aim stays
/// where it was. Multiplying the configured bitrate for the duration moves the
/// pair, through the setBitrate() path every encoder already implements —
/// no new vendor code, no second rate-control mode to keep in step.
///
/// Six, and a ceiling. Six covers the three-to-five a sharp desktop wants with a
/// margin; the ceiling keeps a burst on a very high bitrate stream from turning
/// into a frame no link can move in reasonable time.
constexpr int kStillBoost = 6;
constexpr int kStillMaxKbps = 500000;

inline int stillBitrateKbps(int streamKbps)
{
    if (streamKbps <= 0) return streamKbps;
    const int64_t boosted = static_cast<int64_t>(streamKbps) * kStillBoost;
    return static_cast<int>(boosted > kStillMaxKbps ? kStillMaxKbps : boosted);
}

} // namespace mw::native::encode
