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

#include <cstddef>
#include <cstdint>

namespace mw::native {

/// One encoded video frame, handed to the consumer by pointer into the
/// encoder's own output buffer.
///
/// **Lifetime**: `data` is valid ONLY for the duration of the callback. The
/// consumer either copies it or sends it synchronously; the engine unlocks the
/// encoder bitstream as soon as the callback returns. This is deliberate — it
/// is what keeps the GPU→CPU copy at exactly one (§7) instead of adding an
/// allocation per frame the way a std::vector return would.
///
/// **Format**: Annex-B for H.264/HEVC (start codes, SPS/PPS/VPS ahead of every
/// keyframe), OBU stream for AV1 — byte-for-byte what Sunshine produces today,
/// which is why the browser's decode path needs no change at all.
struct EncodedFrame
{
    const uint8_t* data = nullptr;
    size_t size = 0;

    /// True for an IDR / keyframe. The relay's delta gate keys off this, and a
    /// keyframe is never dropped by any queue on the way out.
    bool keyframe = false;

    /// Monotonic, gapless, starting at 0 for each session. The browser uses it
    /// to spot a hole; the engine uses it for reference invalidation (§9.2).
    uint32_t frameNumber = 0;

    // ── Timestamps, in microseconds on a steady clock ────────────────────────
    //
    // These travel WITH the frame and are never re-read from a shared atomic at
    // drain time. That is not a style preference: MoonlightShim learned the hard
    // way that a drained burst then shares one timestamp, which defeats the
    // frontend's out-of-order filter on a reordering link.

    /// When the OS says the frame was actually presented on the display. On
    /// Windows this is DXGI's LastPresentTime — a real measurement, not an
    /// arrival-time guess. It is the t₀ every latency figure is measured from.
    int64_t presentUs = 0;

    /// When capture handed the surface over (t₁).
    int64_t capturedUs = 0;

    /// When the surface was handed to the colour converter — right after the
    /// acquire, so in practice t₁ again. Kept apart from capturedUs because a
    /// re-send of a still picture has no capture at all, and then this is the
    /// only "start of work" stamp the frame has.
    int64_t submittedUs = 0;

    /// When the conversion pass had been issued and the encoder was about to
    /// be asked (t₂). `convertedUs - submittedUs` is the CPU side of the
    /// colour pass; the GPU side is paid inside the encode that follows.
    int64_t convertedUs = 0;

    /// When the encoder's bitstream became readable (t₃).
    int64_t encodedUs = 0;

    /// Convenience: the host-side processing latency this frame really cost,
    /// present → encoded. This is what the stats overlay shows, and unlike the
    /// GameStream path it is measured here rather than reported by a third
    /// party we have to trust.
    int64_t processingUs() const { return encodedUs - presentUs; }
};

} // namespace mw::native
