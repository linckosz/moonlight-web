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

#include "mw/native/Capabilities.h"
#include "mw/native/SessionConfig.h"

#include <string>

namespace mw::native {

/// Everything the "Detect → Optimize" half of the engine decides, before a
/// single platform object is created.
struct Selection
{
    const DisplayInfo* display = nullptr;
    const GpuInfo* gpu = nullptr;

    Codec codec = Codec::H264;
    EncoderApi encoder = EncoderApi::None;

    int width = 0;
    int height = 0;
    int fps = 0;

    /// The encoder's rate: the client's, cut in the ratio of the pixels where
    /// the frame came out smaller than the size it was set for (see
    /// bitrateForFrame).
    int bitrateKbps = 0;

    /// 10-bit, and therefore actually HDR. False when HDR was asked for but the
    /// display is in SDR or the encoder has no 10-bit path — the session then
    /// runs SDR rather than failing.
    bool hdr = false;

    /// The encoder would carry HDR on an HDR display — see
    /// SessionInfo::hdrCapable. Independent of the request and of the display's
    /// current mode, which are the two things that change under a session.
    bool hdrCapable = false;

    /// 4:4:4 granted: asked for AND the chosen codec has it on this encoder.
    /// The request steers the codec choice (see select()), so this is false
    /// only when no codec the client and the GPU share can carry 4:4:4 — the
    /// session then runs 4:2:0 rather than failing.
    bool yuv444 = false;

    /// The chosen GPU is not the one driving the display, so each frame costs a
    /// cross-GPU copy (§6). Only ever true when the display's own GPU has no
    /// encoder at all.
    bool crossGpuCopy = false;

    /// No GPU in this machine could encode, so `encoder` names a machine-level
    /// fallback (Capabilities::fallbacks) rather than something on `gpu`.
    ///
    /// `gpu` still points at the display's own adapter when there is one: it is
    /// what capture and colour conversion run on, and on a Media Foundation
    /// hardware transform it is also where the frames stay. Only the encoder
    /// moved. Never true while any GPU can encode.
    bool fallbackEncoder = false;

    /// True when that fallback is really running on the CPU, rather than on
    /// fixed-function silicon the OS lent us without a vendor SDK. The one thing
    /// the difference changes downstream is what a session may promise: a CPU
    /// encoder is the case that has to watch whether it is keeping up.
    bool cpuEncoder = false;
};

/// Resolve a SessionConfig against what the machine has.
///
/// Pure: no OS calls, no allocation beyond the error string, no state. That is
/// what lets the whole "which GPU, which encoder, which codec" policy — the
/// part users would otherwise have to configure — be unit-tested on any machine
/// including CI, with no GPU present.
///
/// Returns false and fills `error` when the display does not exist, or when the
/// client and the machine share no codec at all.
bool select(const Capabilities& caps, const SessionConfig& config, Selection& out,
            std::string& error);

/// A frame size and a display size, in pixels.
struct FrameSize
{
    int width = 0;
    int height = 0;
};

/// How the size the client asked for is read — the two SessionConfig flags
/// that qualify it, gathered so every caller of frameForDisplay() passes the
/// same pair (see policyOf()).
struct FramePolicy
{
    /// The requested size is a box to fit, not a height to keep — see
    /// SessionConfig::fitRequestedBox.
    bool fitBox = false;
    /// The frame may be larger than the display — see
    /// SessionConfig::allowUpscale. Read only with fitBox.
    bool allowUpscale = false;
};

/// The frame a display of @p display should be streamed at, given the frame
/// the client asked for. By default: the display's shape at the frame's
/// height, width kept even. The frame is returned untouched when its shape is
/// already within 0.5% of the display's, so a client's even-width rounding is
/// never fought over, and when either size is unknown.
///
/// With FramePolicy::fitBox the frame is instead the largest one of the
/// display's shape that fits inside the requested size — a 16:9 display asked
/// for a 1920x1200 box streams 1920x1080, so the client shows it 1:1 with its
/// own bars around, never a squeezed picture.
///
/// Never larger than the display: a 1440p request of a 1080p display streams
/// 1080p, whatever the encoder (see select()). The one exception is
/// FramePolicy::allowUpscale, which a client asking for its own screen's
/// size sets on purpose. Given the size the client asked for rather than the
/// one the session has now, a display that shrank and grew back returns to
/// that size.
///
/// Pure, shared by select() at the start of a session and by every platform
/// session when the display changes mode under it.
FrameSize frameForDisplay(FrameSize display, FrameSize frame, FramePolicy policy = {});

/// The rate for the frame select() settled on, from the client's @p kbps set
/// for the frame it @p asked: cut in the ratio of their pixels when @p shaped
/// is smaller, untouched otherwise. A 1440p rate asked of a 1080p display
/// would spend 1.8× the bits on each of its pixels: the picture is the same,
/// the link and the encoder carry more (the shared feed on the N95,
/// 21/11/2026). Never raised for a larger frame, never cut below 1000 kbps
/// (nor a rate already below it), and untouched when either size is unknown.
///
/// Pure.
int bitrateForFrame(int kbps, FrameSize asked, FrameSize shaped);

/// The largest frame of whole @p block × @p block blocks that fits inside
/// @p frame AND keeps its shape — what an encoder that cannot express a
/// partial block has to be given.
///
/// Rounding each side on its own is what a naive aligner does, and it stretches
/// the picture: 1366x768 becomes 1360x768, a desktop 0.4% wider than it is.
/// Here both sides move together, so the same frame becomes 1352x760 — a few
/// pixels smaller, the exact shape of the desktop, and nothing on screen leans.
/// A frame already made of whole blocks comes back untouched, which is every
/// common resolution (1920x1080, 2560x1440, 3840x2160).
///
/// Never returns less than one block on a side. @p block must be a power of
/// two; anything else, or a frame with no size, comes back unchanged.
FrameSize alignedToBlocks(FrameSize frame, int block);

/// The same for an encoder whose blocks are not square: the largest frame
/// whose width is a whole number of @p blockWidth and height of
/// @p blockHeight, inside @p frame, closest to its shape — exactly it where
/// the grid allows. The 780M's AV1 encoder codes 64×16 (C13.12): 1920x1080
/// becomes 1792x1008, the same 16:9; 1280x720, 2560x1440 and 3840x2160 are
/// whole already. An encoder told a size off its grid codes a larger frame
/// and says the picture's in AV1's render size — which Chrome ignores: it
/// shows the whole frame, padding and all (05/10/2026).
///
/// Never less than one block on a side; a block below 1, or a frame with no
/// size, comes back unchanged.
FrameSize alignedToGrid(FrameSize frame, int blockWidth, int blockHeight);

/// The FramePolicy a session's config asks for.
inline FramePolicy policyOf(const SessionConfig& config)
{
    return FramePolicy{config.fitRequestedBox, config.fitRequestedBox && config.allowUpscale};
}

/// "Match my screen" could not be honoured: the config becomes Auto's — the
/// fallback box (or the requested one) to fit, no upscale, no mode to match.
/// Returns false when there was nothing to fall back from. The caller then
/// reshapes `width` × `height` against its display (frameForDisplay), as the
/// Selector would have.
bool fallBackFromMatch(SessionConfig& config);

/// The size a display the stream makes for itself may be made at: the bounds
/// the server pins a virtual display's mode to (VirtualDisplay::normaliseMode),
/// even on both sides.
constexpr int kMadeDisplayMin = 480;
constexpr int kMadeDisplayMax = 4096;

/// "Match my screen" on the portal's virtual monitor (Linux). That display has
/// no mode to change and no size of its own — the stream makes it
/// (PortalCapture::setVirtualMonitor) — so the request is honoured by making
/// it at the client's size: the display @p config names takes that size,
/// pinned to [kMadeDisplayMin, kMadeDisplayMax] and made even, and the config
/// stops asking for a mode change. select() then shapes nothing and the frame
/// is the client's screen to the pixel. Returns false, changing nothing, for
/// any other display or a config that did not ask — the caller falls back
/// from the match as before.
bool sizeVirtualMonitorToClient(Capabilities& caps, SessionConfig& config);

} // namespace mw::native
