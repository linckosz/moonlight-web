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

#include "../../capture/linux/KmsCapture.h"
#include "../../encode/OpenH264Encoder.h"
#include "../CursorBlend.h"
#include "../CursorDraw.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mw::native::convert {

/// The scanout buffer to I420, on the CPU — for a Linux machine with no render
/// node.
///
/// ── How the pixels get here ─────────────────────────────────────────────────
///
/// KmsCapture is unchanged: it still names the framebuffer the display is
/// showing and exports it as a DMA-BUF. What changes is who reads it. With a
/// render node the fd goes to EGL (GlConvert); without one it is mmap()ed, which
/// is legal for exactly the buffers a render-node-less machine has — a virtual
/// display adapter's, linear and system-memory-backed. Measured on the Hyper-V
/// bench (07/09/2026): XR24, modifier 0, the whole 1080p picture readable, real
/// pixels. A real GPU's tiled scanout buffer refuses the mmap (amdgpu: EPERM),
/// and that machine has EGL anyway.
///
/// So this class needs the buffer to be LINEAR and says so when it is not,
/// rather than decode a tiled layout it cannot know.
///
/// ── What it costs ───────────────────────────────────────────────────────────
///
/// One pass over the picture, reading the mapping and writing the three planes;
/// no intermediate copy. Split across cores in row bands (BgraToI420.h).
///
/// ── The pointer ─────────────────────────────────────────────────────────────
///
/// Composited into the planes after the pass, by the same CursorBlend the other
/// platforms use — the shape is prepared once per change and the per-frame cost
/// is the footprint only, a few thousand pixels. It is drawn every convert()
/// rather than saved and restored: unlike macOS, which re-encodes a held
/// compositor buffer, this converter rewrites the planes from the scanout each
/// time, so there is nothing of the previous pointer left to undo.
///
/// KMS gives ARGB8888, which DRM defines as PREMULTIPLIED — the convention
/// CursorBlend prepares for. ⚠️ The GL path's shader uses the straight-alpha
/// formula on the same data (`mix(rgb, cursor.rgb, cursor.a)`), so the two
/// differ very slightly on antialiased edges. Noted rather than aligned blind:
/// only a side-by-side on one machine can say which is right, and neither is
/// visibly wrong.
///
/// `CursorState::invert` is ignored, because on this platform it is always
/// zero: inverting shapes are a Windows GDI notion, and a DRM cursor plane has
/// no such thing.
class CpuConvert
{
public:
    CpuConvert() = default;
    ~CpuConvert();

    CpuConvert(const CpuConvert&) = delete;
    CpuConvert& operator=(const CpuConvert&) = delete;

    /// @p sourceFourcc must be XRGB8888 / ARGB8888 / XBGR8888 / ABGR8888.
    /// Output dimensions are rounded down to even.
    bool init(uint32_t sourceFourcc, int sourceWidth, int sourceHeight, int outputWidth,
              int outputHeight, std::string& error);

    /// Read @p frame's first plane and write the planes picture() describes.
    bool convert(const capture::KmsFrame& frame, const capture::CursorState& cursor,
                 const CursorDraw& draw, std::string& error);

    const encode::I420Picture& picture() const { return m_Picture; }
    int outputWidth() const { return m_OutputWidth; }
    int outputHeight() const { return m_OutputHeight; }
    int threads() const { return m_Threads; }

private:
    /// Composite the pointer into the planes just written. Called at the end of
    /// convert(), never on its own: it assumes the planes hold this frame.
    void blendPointer(const capture::CursorState& cursor, const CursorDraw& draw);

    bool m_RgbOrder = false;
    int m_SourceWidth = 0;
    int m_SourceHeight = 0;
    int m_OutputWidth = 0;
    int m_OutputHeight = 0;
    int m_Threads = 1;
    std::vector<uint8_t> m_Planes;
    encode::I420Picture m_Picture;

    /// The pointer, prepared into I420 code values. Rebuilt only when the shape
    /// changes — CursorState::shapeVersion is what says so, and a moving
    /// pointer keeps its shape for thousands of frames.
    PreparedCursor m_Prepared;
    uint64_t m_PreparedVersion = 0;
    bool m_PreparedEmpty = true;
};

} // namespace mw::native::convert
