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

#include "Capabilities.h"
#include "EncoderTuning.h"

#include <cstdint>
#include <vector>

namespace mw::native {

/// What one streaming session asks for. Deliberately small: every knob Sunshine
/// exposes (GOP, B-frames, preset, tuning, lookahead, VBV, QP) is decided by the
/// engine and is absent here on purpose (§28 of the mission).
///
/// The fields that ARE here all come from choices the user already makes today,
/// in the stream settings shared by every host — plus the display they clicked.
struct SessionConfig
{
    /// Which display to stream — DisplayInfo::id. The one and only technical
    /// choice a user makes.
    int displayId = -1;

    /// Requested output size. Zero means "native resolution of the display",
    /// which is the default. When smaller, the scale happens in the same GPU
    /// pass as the colour conversion, so it costs nothing extra.
    int width = 0;
    int height = 0;

    /// Requested frame rate. Zero means "the display's own refresh rate".
    int fps = 0;

    int bitrateKbps = 20000;

    /// Codecs the BROWSER accepts, best first. The engine intersects this with
    /// what the display's GPU can encode and picks the first survivor — which
    /// is why no codec question is ever asked (§27).
    ///
    /// Empty is a programming error, not a default: an empty list would leave
    /// the engine guessing what the client can decode.
    std::vector<Codec> clientCodecs;

    /// Ask for HDR. Honoured only when the display is actually in an HDR mode
    /// AND the chosen encoder does 10-bit; otherwise the session runs SDR and
    /// says so in SessionInfo, rather than failing.
    bool hdr = false;

    /// Ask for 4:4:4 chroma. MoonlightWeb already offers this for external
    /// hosts, and it matters on a desktop rather than on video: 4:2:0 keeps a
    /// quarter of the colour resolution, which shows on text and thin UI lines.
    /// Honoured only when the encoder supports it — see SessionInfo::yuv444.
    bool yuv444 = false;

    /// Encode with intra-refresh instead of relying on keyframes for recovery.
    ///
    /// Each frame then carries a moving band of intra blocks, so a receiver
    /// that has lost data repairs itself within one refresh cycle rather than
    /// waiting for a fresh keyframe.
    ///
    /// **Only worth asking for when the receiver rides it out.** MoonlightWeb's
    /// default recovery discards deltas and demands an IDR on any gap, which
    /// collects none of that benefit while still paying for it in slightly
    /// larger P-frames. So this is off unless the client says it will keep
    /// decoding through the damage — see SessionInfo::intraRefresh for what was
    /// actually granted.
    bool intraRefresh = false;

    /// Draw the mouse cursor into the captured frame. On by default: the remote
    /// user needs to see where they are pointing.
    bool captureCursor = true;

    /// The client's display refresh, in millihertz, when the browser measured
    /// one. Zero means "unknown". Read together with clientVsync: a client
    /// that presents on its vsync gets a stream cadence that is an integer
    /// divisor of this rate, within a fifth of `fps`, so frames land on its
    /// grid instead of beating against it (§9.11, CadenceAlign.h). Ignored
    /// when `fps` is 0 (the host's own rate) or when the client tears.
    int clientRefreshMilliHz = 0;

    /// True when the client paints frames on its display's vsync — tearing
    /// off, or a browser that cannot tear. False, the default, means frames
    /// are painted the moment they are decoded and there is no grid to align
    /// on: the setting is followed as it is, which is the lowest latency.
    bool clientVsync = false;

    // ── Bench-only, below this line ─────────────────────────────────────────
    //
    // Neither field is ever set by a session a browser started. They exist so
    // `--native-bench` can measure the engine's choices against the
    // alternatives, on hardware the choices were not made on. See EncoderTuning.

    /// Encoder settings to override the engine's own with. Default = the
    /// engine's choice, which is the product.
    EncoderTuning tuning;

    /// Encode on THIS GPU (GpuInfo::id) rather than on the display's own,
    /// paying the cross-GPU copy the Selector otherwise avoids. -1, the
    /// default, lets the Selector decide. How the bench reaches an encoder
    /// that drives no display — an iGPU next to a discrete card, typically.
    int encodeGpuId = -1;
};

/// What the engine settled on once a session started. Mirrored into the stats
/// overlay so the user can see the GPU and encoder actually in use — the only
/// technical information the UI shows (§28).
struct SessionInfo
{
    int displayId = -1;
    int width = 0;
    int height = 0;
    int fps = 0;

    Codec codec = Codec::H264;
    EncoderApi encoder = EncoderApi::None;
    CaptureApi capture = CaptureApi::None;

    /// e.g. "NVIDIA GeForce RTX 4070"
    std::string gpuName;

    /// True when the session really is 10-bit HDR. May be false even though
    /// SessionConfig::hdr was true — see that field.
    bool hdr = false;

    /// True when the session really is 4:4:4. Same contract as `hdr`: asked for
    /// is not granted, and the stats overlay is where the difference shows.
    bool yuv444 = false;

    /// True when the stream really refreshes by intra-refresh rather than by
    /// keyframes. Same contract again — an encoder that cannot do it says so
    /// here, and the receiver must then keep its usual keyframe recovery.
    bool intraRefresh = false;

    /// How many encoded frames one intra-refresh wave takes to sweep the whole
    /// picture — the period the encoder was configured with, in frames of the
    /// rate it encodes at (RateControl.h intraRefreshPeriodFrames). Zero when
    /// intraRefresh is false. The receiver counts the frames it gets against
    /// this to know when a wave has had its chance: in frames, because the
    /// wave advances per frame encoded, not per second — a game presenting at
    /// 30 under a 165 fps stream stretches a 330-frame period to 11 s.
    int intraRefreshFrames = 0;

    /// True when a frame the receiver lost can be healed by an ordinary delta
    /// (Session::invalidateReference does what it says). The receiver then
    /// keeps decoding after a gap instead of discarding deltas until a
    /// keyframe; false means the engine answers an invalidation with an IDR.
    bool referenceInvalidation = false;

    /// Memory copies between capture and the wire, counted rather than
    /// estimated. Logged at session start and compared in the benchmarks (§16);
    /// if this number ever grows, a zero-copy path silently broke.
    int copiesPerFrame = 0;

    /// True when the frame has to cross from the display's GPU to a different
    /// GPU to be encoded (§6). Costly and rare — always worth a log line.
    bool crossGpuCopy = false;

    /// True when the session captures the host's playback and delivers Opus
    /// packets. False when no audio callback was given, or when the platform
    /// could not open a playback device — the stream is then silent, and the
    /// log says why.
    bool audio = false;
};

} // namespace mw::native
