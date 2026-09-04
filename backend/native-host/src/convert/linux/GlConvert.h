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

#include "../../capture/CaptureTypes.h"
#include "../../capture/linux/KmsCapture.h"
#include "../CursorDraw.h"

#include <cstdint>
#include <memory>
#include <string>

// Turns a scanout DMA-BUF into the NV12 surface the encoder owns, on the GPU,
// without touching system memory. The Linux counterpart of ColorConvert.
//
// ── Why GL sits between KMS and VA-API ──────────────────────────────────────
//
// The scanout buffer is tiled — on AMD with DCC compression metadata in two
// extra planes — and radeonsi's VA-API refuses to import that modifier (it
// accepts linear and nothing else, measured on a 780M on 04/09/2026). EGL on
// the same driver lists the very same modifier among its supported imports.
// So the frame goes KMS → EGLImage → a fragment shader → the encoder's surface,
// and the GPU never copies it: the shader READS the tiled buffer in place and
// WRITES the encoder's planes in place.
//
// ── The same trick as D3D11, one API over ───────────────────────────────────
//
// NV12 is planar and GL cannot render into "an NV12 texture" any more than
// D3D11 can. What both allow is a view per plane: the VA surface is exported as
// two DMA-BUF layers, R8 (luma) and GR88 (interleaved chroma), each imported as
// its own EGLImage and attached to its own framebuffer. Two small draws — a
// full-screen triangle, no vertex buffer — and the shader math is ColorConvert's
// transcribed from HLSL to GLSL, BT.709 limited range, cursor composited on the
// way through.

namespace mw::native::convert {

/// The encoder's NV12 surface, as vaExportSurfaceHandle describes it with
/// VA_EXPORT_SURFACE_SEPARATE_LAYERS: one layer per plane.
struct Nv12Target
{
    int width = 0;
    int height = 0;
    uint64_t modifier = 0; ///< of the VA allocation; GL imports it as-is
    int fdY = -1;
    uint32_t offsetY = 0;
    uint32_t pitchY = 0;
    int fdUV = -1;
    uint32_t offsetUV = 0;
    uint32_t pitchUV = 0;
};

class GlConvert
{
public:
    GlConvert();
    ~GlConvert();

    GlConvert(const GlConvert&) = delete;
    GlConvert& operator=(const GlConvert&) = delete;

    /// Bring up EGL on @p renderNode (the GPU that scans the display out — the
    /// same one the encoder runs on) and compile the shaders for a source of
    /// @p sourceFourcc at @p sourceWidth × @p sourceHeight, scaled to
    /// @p outputWidth × @p outputHeight. Output dimensions are rounded down to
    /// even, as NV12 requires.
    bool init(const std::string& renderNode, uint32_t sourceFourcc, int sourceWidth,
              int sourceHeight, int outputWidth, int outputHeight, std::string& error);

    /// Attach the encoder's surface as the render target. Done once per
    /// session, not per frame: the planes are imported and their framebuffers
    /// built here, and convert() then only draws.
    bool bindTarget(const Nv12Target& target, std::string& error);

    /// Convert one frame into the bound target. Imports @p frame's planes as an
    /// EGLImage, draws luma then chroma, and waits for the GPU — the encoder
    /// reads the surface next, on a queue GL knows nothing about, so the wait is
    /// what makes the hand-off correct.
    bool convert(const capture::KmsFrame& frame, const capture::CursorState& cursor,
                 const CursorDraw& draw, std::string& error);

    int outputWidth() const { return m_OutputWidth; }
    int outputHeight() const { return m_OutputHeight; }

    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> d;

    bool createContext(const std::string& renderNode, std::string& error);
    bool createShaders(std::string& error);
    /// Bind the context to the calling thread if it is not already. convert()
    /// runs on the capture thread, init() on whoever built the session.
    bool makeCurrent(std::string& error);

public:
    /// Release the context from the calling thread. A thread that converted
    /// calls this before it ends, so stop() — on another thread — can bind
    /// the context to tear it down. Harmless when nothing is current.
    void detachThread();

private:
    bool updateCursorTextures(const capture::CursorState& cursor, std::string& error);

    uint32_t m_SourceFourcc = 0;
    int m_SourceWidth = 0;
    int m_SourceHeight = 0;
    int m_OutputWidth = 0;
    int m_OutputHeight = 0;
    uint64_t m_CursorShapeVersion = 0;
};

} // namespace mw::native::convert
