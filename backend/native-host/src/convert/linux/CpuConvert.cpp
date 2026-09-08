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

#include "CpuConvert.h"

#include "../../core/Log.h"
#include "../BgraToI420.h"

#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace mw::native::convert {
namespace {

/// Row bands beyond this buy nothing: the pass is memory-bound, and the
/// machines this runs on have four cores.
constexpr int kMaxThreads = 4;

std::string fourccName(uint32_t f)
{
    std::string s;
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((f >> (8 * i)) & 0xFF);
        s.push_back(c >= 32 && c < 127 ? c : '?');
    }
    return s;
}

} // namespace

CpuConvert::~CpuConvert() = default;

bool CpuConvert::init(uint32_t sourceFourcc, int sourceWidth, int sourceHeight, int outputWidth,
                      int outputHeight, std::string& error)
{
    switch (sourceFourcc) {
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ARGB8888: m_RgbOrder = false; break;
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_ABGR8888: m_RgbOrder = true; break;
    default:
        error = "the CPU converter takes 8-bit XRGB/XBGR scanout buffers, not " +
                fourccName(sourceFourcc);
        return false;
    }
    if (sourceWidth <= 0 || sourceHeight <= 0 || outputWidth <= 0 || outputHeight <= 0) {
        error = "empty picture";
        return false;
    }
    m_SourceWidth = sourceWidth;
    m_SourceHeight = sourceHeight;
    m_OutputWidth = outputWidth & ~1;
    m_OutputHeight = outputHeight & ~1;

    const unsigned hw = std::thread::hardware_concurrency();
    m_Threads = static_cast<int>(std::min<unsigned>(hw == 0 ? 1 : hw, kMaxThreads));
    // Never more bands than there are row pairs to give them.
    m_Threads = std::max(1, std::min(m_Threads, m_OutputHeight / 2));

    const size_t lumaBytes = static_cast<size_t>(m_OutputWidth) * m_OutputHeight;
    m_Planes.assign(lumaBytes * 3 / 2, 0);
    m_Picture.y = m_Planes.data();
    m_Picture.u = m_Picture.y + lumaBytes;
    m_Picture.v = m_Picture.u + lumaBytes / 4;
    m_Picture.strideY = m_OutputWidth;
    m_Picture.strideU = m_Picture.strideV = m_OutputWidth / 2;
    m_Picture.width = m_OutputWidth;
    m_Picture.height = m_OutputHeight;

    log::info("[native] colour conversion: " + std::to_string(sourceWidth) + "x" +
              std::to_string(sourceHeight) + " " + fourccName(sourceFourcc) + " -> " +
              std::to_string(m_OutputWidth) + "x" + std::to_string(m_OutputHeight) +
              " I420 4:2:0 (BT.709 limited) on the CPU, " + std::to_string(m_Threads) +
              " thread(s), from a DMA-BUF mapping");
    return true;
}

bool CpuConvert::convert(const capture::KmsFrame& frame, const capture::CursorState& cursor,
                         const CursorDraw& draw, std::string& error)
{
    (void)draw;
    if (frame.planeCount < 1 || frame.fds[0] < 0) {
        error = "the frame has no plane to read";
        return false;
    }
    // Only a linear buffer has a layout this code knows. A modifier here means
    // a real GPU's tiled scanout — and a real GPU has a render node, so the
    // Selector would not have sent it here.
    if (frame.modifier != 0 && frame.modifier != DRM_FORMAT_MOD_LINEAR) {
        error = "the scanout buffer is tiled (modifier 0x" + [&] {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%llx",
                          static_cast<unsigned long long>(frame.modifier));
            return std::string(buf);
        }() + ") — the CPU converter reads linear buffers only";
        return false;
    }
    if (frame.width != m_SourceWidth || frame.height != m_SourceHeight) {
        error = "the frame is " + std::to_string(frame.width) + "x" + std::to_string(frame.height) +
                ", the converter was set up for " + std::to_string(m_SourceWidth) + "x" +
                std::to_string(m_SourceHeight);
        return false;
    }

    // The whole first plane. Mapped per frame: the fds are per frame (the
    // capture closes them when the next buffer replaces this one), and a
    // mapping costs far less than the pass that follows it.
    const size_t length = static_cast<size_t>(frame.offsets[0]) +
                          static_cast<size_t>(frame.pitches[0]) * static_cast<size_t>(frame.height);
    void* map = ::mmap(nullptr, length, PROT_READ, MAP_SHARED, frame.fds[0], 0);
    if (map == MAP_FAILED) {
        error = "the scanout buffer cannot be mapped by the CPU (" + std::string(strerror(errno)) +
                ") — a tiled or device-local buffer";
        return false;
    }

    // Coherency with whoever wrote the buffer: best effort, the exporter may
    // not implement it, and a linear shmem buffer needs nothing.
    dma_buf_sync sync = {};
    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
    ::ioctl(frame.fds[0], DMA_BUF_IOCTL_SYNC, &sync);

    BgraToI420Params p;
    p.src = static_cast<const uint8_t*>(map) + frame.offsets[0];
    p.srcPitch = frame.pitches[0];
    p.srcWidth = m_SourceWidth;
    p.srcHeight = m_SourceHeight;
    p.rgbOrder = m_RgbOrder;
    p.y = m_Planes.data();
    p.u = const_cast<uint8_t*>(m_Picture.u);
    p.v = const_cast<uint8_t*>(m_Picture.v);
    p.dstWidth = m_OutputWidth;
    p.dstHeight = m_OutputHeight;
    p.strideY = m_Picture.strideY;
    p.strideUV = m_Picture.strideU;

    if (m_Threads <= 1) {
        bgraToI420Rows(p, 0, m_OutputHeight);
    } else {
        // Row bands, even-aligned, one thread each. Spawned per frame: a
        // hundred microseconds against a pass of a few milliseconds, and no
        // pool to keep alive on a machine that may not stream for hours.
        const int pairs = m_OutputHeight / 2;
        const int perBand = (pairs + m_Threads - 1) / m_Threads;
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(m_Threads));
        for (int t = 0; t < m_Threads; ++t) {
            const int begin = t * perBand * 2;
            const int end = std::min(m_OutputHeight, (t + 1) * perBand * 2);
            if (begin >= end) break;
            workers.emplace_back([&p, begin, end] { bgraToI420Rows(p, begin, end); });
        }
        for (std::thread& w : workers)
            w.join();
    }

    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    ::ioctl(frame.fds[0], DMA_BUF_IOCTL_SYNC, &sync);
    ::munmap(map, length);

    blendPointer(cursor, draw);
    return true;
}

void CpuConvert::blendPointer(const capture::CursorState& cursor, const CursorDraw& draw)
{
    if (!cursor.visible || cursor.inkWidth <= 0 || cursor.width <= 0 || cursor.height <= 0) return;
    if (m_SourceWidth <= 0 || m_SourceHeight <= 0) return;

    if (cursor.shapeVersion != m_PreparedVersion || m_PreparedEmpty) {
        m_Prepared = prepareCursor(cursor.pixels.data(), cursor.width, cursor.height,
                                   BlendTarget::Nv12Bt709);
        m_PreparedVersion = cursor.shapeVersion;
        m_PreparedEmpty = m_Prepared.empty();
    }
    if (m_Prepared.empty()) return;

    // The shape's position is in captured-frame pixels and the planes are in
    // output pixels, so everything is scaled by the same ratio the colour pass
    // used. The magnification grows the pointer around its HOTSPOT, so the tip
    // of an enlarged arrow stays where the real one is — the same arithmetic as
    // the GL path, written in pixels instead of normalised coordinates.
    const float sx = static_cast<float>(m_OutputWidth) / static_cast<float>(m_SourceWidth);
    const float sy = static_cast<float>(m_OutputHeight) / static_cast<float>(m_SourceHeight);
    const float magnify = draw.magnify > 1.0f ? draw.magnify : 1.0f;

    CursorPlacement place;
    place.scale = magnify * sx;
    place.x = (static_cast<float>(cursor.x + draw.hotspotX) -
               static_cast<float>(draw.hotspotX) * magnify) *
              sx;
    place.y = (static_cast<float>(cursor.y + draw.hotspotY) -
               static_cast<float>(draw.hotspotY) * magnify) *
              sy;

    PlaneViews planes;
    planes.y = m_Planes.data();
    planes.yStride = static_cast<size_t>(m_Picture.strideY);
    planes.uv = const_cast<uint8_t*>(m_Picture.u);
    planes.uvStride = static_cast<size_t>(m_Picture.strideU);
    planes.v = const_cast<uint8_t*>(m_Picture.v);
    planes.vStride = static_cast<size_t>(m_Picture.strideV);
    planes.width = m_OutputWidth;
    planes.height = m_OutputHeight;
    blendCursor(m_Prepared, place, planes);
}

} // namespace mw::native::convert
