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
/// ── What it does not do ─────────────────────────────────────────────────────
///
/// The pointer is not drawn into the picture on this path: a client that draws
/// its own (the desktop default) is unaffected; the composited mode used for
/// games gets no pointer here for now, said once in the log.
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
    bool m_RgbOrder = false;
    int m_SourceWidth = 0;
    int m_SourceHeight = 0;
    int m_OutputWidth = 0;
    int m_OutputHeight = 0;
    int m_Threads = 1;
    std::vector<uint8_t> m_Planes;
    encode::I420Picture m_Picture;
    bool m_CursorNoted = false;
};

} // namespace mw::native::convert
