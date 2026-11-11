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
#include <vector>

// What a hardware Media Foundation transform is asked again when it refuses
// to start (A121, issue #54's second report).
//
// Qualcomm's H.264 and HEVC transforms on a Snapdragon 845 accepted their
// types, then refused to start streaming — E_INVALIDARG, at 1920x960 and at
// 540x1080, always at 120 fps, with the one-frame VBV of 30 KB — and the
// session fell to Microsoft's software H.264, 30 to 52 fps. The same settings
// start on a Snapdragon 7c's transform (mw-arm, 11/11/2026), so what that
// transform refuses is not known. Two candidates are cheap to try before the
// CPU: a cadence it was certified for, and a buffer it is used to.
//
// So, after the settings the session asked for:
//
//  1. the same transform DECLARED at 60 fps. The frames still come at the
//     stream's rate and are encoded one by one; only the rate the transform
//     plans its bits by is 60. Its time stamps advance by a sixtieth of a
//     second a frame, so its own idea of a second is that many frames, and
//     the bitrate it is given is scaled by declared / stream rate — 15 Mbit/s
//     at 120 fps is asked as 7.5 Mbit/s for "60 fps", and comes out at 15 on
//     the wire. The VBV keeps its size in bits: it fills at that scaled rate
//     for that many frames a "second", which is the wire's rate in real time,
//     so the same bits hold the same time — a VBV recomputed at the declared
//     rate would be half as deep, the opposite of a gentler ask;
//  2. the same, with a buffer of four frames of the stream instead of the
//     usual one: the latency the rule saves is worth less than a hardware
//     encoder.
//
// Only a refusal BEFORE the first picture is answered this way: of its types
// or of its start. A transform that starts and then says nothing (AMD's on
// DualRTX, 07/09/2026) is not helped by a slower clock, and each of its tries
// costs a timeout.
//
// Pure, no Windows: tested on every OS.

namespace mw::native::encode {

/// One bring-up of a hardware transform.
struct MfStartAttempt
{
    /// The rate the transform is told: its frame rate and time stamps.
    int declaredFps = 60;
    /// EncoderTuning::vbvFrames for it, in frames of the STREAM's rate: 0 for
    /// the one-frame rule (RateControl.h's vbvBits at the stream's rate).
    int vbvFrames = 0;
    /// For the log: what this attempt changed, empty for the first.
    const char* change = "";
};

/// The cadence a transform is retried at.
constexpr int kMfRetryFps = 60;
/// The buffer it is retried with, in frames of the declared rate.
constexpr int kMfRetryVbvFrames = 4;

/// The attempts, first to last: the session's own settings, then the ones
/// above where they change anything. A stream at 60 fps or less keeps its
/// cadence; a VBV the bench already widened is not narrowed.
inline std::vector<MfStartAttempt> mfStartLadder(int streamFps, int vbvFrames)
{
    const int fps = streamFps > 0 ? streamFps : 60;
    std::vector<MfStartAttempt> ladder;
    ladder.push_back(MfStartAttempt{fps, vbvFrames, ""});
    const int slower = fps > kMfRetryFps ? kMfRetryFps : fps;
    if (slower != fps) ladder.push_back(MfStartAttempt{slower, vbvFrames, "declared at 60 fps"});
    if (vbvFrames < kMfRetryVbvFrames)
        ladder.push_back(
            MfStartAttempt{slower, kMfRetryVbvFrames,
                           slower != fps ? "declared at 60 fps, a 4-frame VBV" : "a 4-frame VBV"});
    return ladder;
}

/// The bitrate to give a transform declared at @p declaredFps for a stream at
/// @p streamFps, so that the wire carries @p bitrateKbps: its second is
/// declaredFps frames of the stream.
inline int mfDeclaredKbps(int bitrateKbps, int declaredFps, int streamFps)
{
    if (declaredFps <= 0 || streamFps <= 0 || declaredFps == streamFps) return bitrateKbps;
    const int64_t scaled = static_cast<int64_t>(bitrateKbps) * declaredFps / streamFps;
    return scaled > 0 ? static_cast<int>(scaled) : 1;
}

} // namespace mw::native::encode
