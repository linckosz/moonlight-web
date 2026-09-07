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

// 8-bit BGRA (or RGBA) to planar 4:2:0 on the CPU — the colour conversion of a
// machine that has no GPU to do it on.
//
// ── Where it is used ────────────────────────────────────────────────────────
//
// The Linux CPU pipeline: a virtual machine whose display adapter has no render
// node (hyperv_drm) hands its scanout buffer to the CPU through a DMA-BUF mmap,
// and this is what turns those pixels into what OpenH264 eats. Pure arithmetic
// over memory, no platform, so it is tested on Windows too.
//
// ── Colour ──────────────────────────────────────────────────────────────────
//
// BT.709, limited range — what every SDR stream in this engine carries and what
// the encoders' VUI declares. Integer, one multiply-add per channel per plane;
// the coefficients are the usual 8-bit studio-range set (×219/255 for luma,
// ×224/255 for chroma, scaled by 256).
//
// ── Scaling ─────────────────────────────────────────────────────────────────
//
// 1:1 is the fast path and the common one (the VM's display IS the stream's
// size). Any other ratio samples bilinearly at the mapped coordinate, in 16.16
// fixed point: four taps a pixel, which on four cores is a couple of
// milliseconds at 1080p — and cheaper than the encode it feeds.
//
// Work is handed out in ROWS so a caller can split a picture across threads:
// bgraToI420Rows(params, 0, h/2) and (h/2, h) produce the same bytes as one call
// over (0, h). Rows are counted in DESTINATION lines and rounded to even pairs,
// because a 4:2:0 chroma sample covers two of them.

namespace mw::native::convert {

struct BgraToI420Params
{
    const uint8_t* src = nullptr;
    size_t srcPitch = 0; ///< bytes per source row
    int srcWidth = 0;
    int srcHeight = 0;
    /// Byte order of a source pixel: false = B,G,R,X (DRM XRGB8888 /
    /// DXGI B8G8R8A8 in memory), true = R,G,B,X (DRM XBGR8888).
    bool rgbOrder = false;

    uint8_t* y = nullptr;
    uint8_t* u = nullptr;
    uint8_t* v = nullptr;
    int dstWidth = 0;  ///< even
    int dstHeight = 0; ///< even
    int strideY = 0;   ///< bytes per row of the luma plane
    int strideUV = 0;  ///< bytes per row of each chroma plane
};

namespace detail {

inline uint8_t clampByte(int v)
{
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/// One pixel's luma, BT.709 limited.
inline uint8_t lumaOf(int r, int g, int b)
{
    return clampByte(((47 * r + 157 * g + 16 * b + 128) >> 8) + 16);
}

/// Chroma of a 2×2 block from its summed RGB (four pixels), BT.709 limited.
inline void chromaOf(int sumR, int sumG, int sumB, uint8_t& cb, uint8_t& cr)
{
    // Averaging first (>> 2) keeps the arithmetic in 32 bits with room to spare.
    // Each row of coefficients sums to exactly zero, so a grey — white
    // included — lands on 128 rather than 127: −26 −87 +113 and 112 −102 −10.
    const int r = sumR >> 2, g = sumG >> 2, b = sumB >> 2;
    cb = clampByte(((-26 * r - 87 * g + 113 * b + 128) >> 8) + 128);
    cr = clampByte(((112 * r - 102 * g - 10 * b + 128) >> 8) + 128);
}

/// Bilinear sample of the source at a 16.16 fixed-point coordinate.
inline void sampleBilinear(const BgraToI420Params& p, int64_t fx, int64_t fy, int& r, int& g,
                           int& b)
{
    int x0 = static_cast<int>(fx >> 16);
    int y0 = static_cast<int>(fy >> 16);
    const int ax = static_cast<int>(fx & 0xFFFF) >> 8; // 0..255
    const int ay = static_cast<int>(fy & 0xFFFF) >> 8;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    const int x1 = x0 + 1 < p.srcWidth ? x0 + 1 : x0;
    const int y1 = y0 + 1 < p.srcHeight ? y0 + 1 : y0;
    if (x0 >= p.srcWidth) x0 = p.srcWidth - 1;
    if (y0 >= p.srcHeight) y0 = p.srcHeight - 1;

    const uint8_t* p00 = p.src + static_cast<size_t>(y0) * p.srcPitch + static_cast<size_t>(x0) * 4;
    const uint8_t* p10 = p.src + static_cast<size_t>(y0) * p.srcPitch + static_cast<size_t>(x1) * 4;
    const uint8_t* p01 = p.src + static_cast<size_t>(y1) * p.srcPitch + static_cast<size_t>(x0) * 4;
    const uint8_t* p11 = p.src + static_cast<size_t>(y1) * p.srcPitch + static_cast<size_t>(x1) * 4;
    const int w00 = (256 - ax) * (256 - ay), w10 = ax * (256 - ay), w01 = (256 - ax) * ay,
              w11 = ax * ay;
    const int ri = p.rgbOrder ? 0 : 2, bi = p.rgbOrder ? 2 : 0;
    r = (p00[ri] * w00 + p10[ri] * w10 + p01[ri] * w01 + p11[ri] * w11 + 32768) >> 16;
    g = (p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11 + 32768) >> 16;
    b = (p00[bi] * w00 + p10[bi] * w10 + p01[bi] * w01 + p11[bi] * w11 + 32768) >> 16;
}

} // namespace detail

/// Convert destination rows [rowBegin, rowEnd) — both even — of the picture
/// described by @p p. Safe to call concurrently for disjoint row ranges.
inline void bgraToI420Rows(const BgraToI420Params& p, int rowBegin, int rowEnd)
{
    using namespace detail;
    rowBegin &= ~1;
    rowEnd &= ~1;
    if (rowEnd > p.dstHeight) rowEnd = p.dstHeight;
    const int ri = p.rgbOrder ? 0 : 2, bi = p.rgbOrder ? 2 : 0;

    if (p.srcWidth == p.dstWidth && p.srcHeight == p.dstHeight) {
        // ── 1:1 ─────────────────────────────────────────────────────────────
        for (int row = rowBegin; row < rowEnd; row += 2) {
            const uint8_t* s0 = p.src + static_cast<size_t>(row) * p.srcPitch;
            const uint8_t* s1 = s0 + p.srcPitch;
            uint8_t* y0 = p.y + static_cast<size_t>(row) * p.strideY;
            uint8_t* y1 = y0 + p.strideY;
            uint8_t* cb = p.u + static_cast<size_t>(row / 2) * p.strideUV;
            uint8_t* cr = p.v + static_cast<size_t>(row / 2) * p.strideUV;
            for (int x = 0; x < p.dstWidth; x += 2) {
                const uint8_t* a = s0 + static_cast<size_t>(x) * 4;
                const uint8_t* b = a + 4;
                const uint8_t* c = s1 + static_cast<size_t>(x) * 4;
                const uint8_t* d = c + 4;
                y0[x] = lumaOf(a[ri], a[1], a[bi]);
                y0[x + 1] = lumaOf(b[ri], b[1], b[bi]);
                y1[x] = lumaOf(c[ri], c[1], c[bi]);
                y1[x + 1] = lumaOf(d[ri], d[1], d[bi]);
                chromaOf(a[ri] + b[ri] + c[ri] + d[ri], a[1] + b[1] + c[1] + d[1],
                         a[bi] + b[bi] + c[bi] + d[bi], cb[x / 2], cr[x / 2]);
            }
        }
        return;
    }

    // ── Scaled: bilinear at the mapped centre of each destination pixel ────
    const int64_t stepX = (static_cast<int64_t>(p.srcWidth) << 16) / p.dstWidth;
    const int64_t stepY = (static_cast<int64_t>(p.srcHeight) << 16) / p.dstHeight;
    const int64_t half = 1 << 15;
    for (int row = rowBegin; row < rowEnd; row += 2) {
        uint8_t* y0 = p.y + static_cast<size_t>(row) * p.strideY;
        uint8_t* y1 = y0 + p.strideY;
        uint8_t* cb = p.u + static_cast<size_t>(row / 2) * p.strideUV;
        uint8_t* cr = p.v + static_cast<size_t>(row / 2) * p.strideUV;
        const int64_t fy0 = row * stepY + (stepY >> 1) - half;
        const int64_t fy1 = fy0 + stepY;
        for (int x = 0; x < p.dstWidth; x += 2) {
            const int64_t fx0 = x * stepX + (stepX >> 1) - half;
            const int64_t fx1 = fx0 + stepX;
            int r[4], g[4], b[4];
            sampleBilinear(p, fx0 < 0 ? 0 : fx0, fy0 < 0 ? 0 : fy0, r[0], g[0], b[0]);
            sampleBilinear(p, fx1, fy0 < 0 ? 0 : fy0, r[1], g[1], b[1]);
            sampleBilinear(p, fx0 < 0 ? 0 : fx0, fy1, r[2], g[2], b[2]);
            sampleBilinear(p, fx1, fy1, r[3], g[3], b[3]);
            y0[x] = lumaOf(r[0], g[0], b[0]);
            y0[x + 1] = lumaOf(r[1], g[1], b[1]);
            y1[x] = lumaOf(r[2], g[2], b[2]);
            y1[x + 1] = lumaOf(r[3], g[3], b[3]);
            chromaOf(r[0] + r[1] + r[2] + r[3], g[0] + g[1] + g[2] + g[3],
                     b[0] + b[1] + b[2] + b[3], cb[x / 2], cr[x / 2]);
        }
    }
}

} // namespace mw::native::convert
