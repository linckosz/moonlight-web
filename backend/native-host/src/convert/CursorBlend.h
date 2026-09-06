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

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Drawing a pointer into a YCbCr picture on the CPU.
//
// ── Why this exists when the GPU converters already draw the pointer ────────
//
// On Windows and Linux the capture hands out a desktop WITHOUT the pointer and
// a shader composites it while converting to NV12 — magnified when a phone asks
// (CursorDraw). On macOS there is no converter: ScreenCaptureKit writes the
// encoder's NV12 itself, pointer included, at the pointer's own size. To
// magnify it the capture has to be told to leave the pointer out, and the shape
// AppKit reports has to be drawn into the compositor's buffer before it is
// encoded. That buffer is a few hundred pixels of pointer inside a 4K frame, so
// a CPU blend of the pointer's footprint is the cheap way, and the only one
// that adds no pass over the picture.
//
// Pure arithmetic over planes, no platform: tested on every OS.
//
// ── Colour ──────────────────────────────────────────────────────────────────
//
// The shape arrives as premultiplied sRGB BGRA (what CGBitmapContext gives).
// It is prepared ONCE per shape into the target's own code values, already
// multiplied by coverage, so the per-frame work is a scaled sample and one
// multiply-add per plane: out = dst × (1 − a) + contribution. Two targets:
//
//  - NV12, BT.709 limited range (the SDR stream): luma and chroma from the
//    gamma-encoded sRGB values, as every SDR pipeline does;
//  - P010, BT.2020 PQ limited range (the HDR stream): sRGB → linear light →
//    BT.2020 primaries → 203 nits for SDR white → PQ → the NCL matrix ON THE PQ
//    SIGNAL, in that order (design §16.1) — a pointer blended in gamma space on
//    a PQ picture comes out far too dark.
//
// Alpha is applied to code values, not to light. For an anti-aliased edge a
// pixel wide that is invisible; for the maths to be exact one would blend in
// linear light and re-encode, which is a whole pass on every touched pixel and
// not worth it for a pointer.

namespace mw::native::convert {

enum class BlendTarget
{
    Nv12Bt709,    ///< 8-bit 4:2:0, BT.709, limited range
    P010Bt2020Pq, ///< 10-bit-in-16 4:2:0, BT.2020 NCL on a PQ signal, limited range
};

/// A shape prepared for one target: per source pixel the luma and chroma code
/// contributions (already × coverage) and the coverage itself.
struct PreparedCursor
{
    int width = 0;
    int height = 0;
    /// Bounding box of the covered pixels, from the top-left: what a pointer
    /// is sized on (CursorState::inkWidth explains why not the canvas).
    int inkWidth = 0;
    int inkHeight = 0;
    BlendTarget target = BlendTarget::Nv12Bt709;
    std::vector<float> luma;  ///< width × height, code units × coverage
    std::vector<float> cb;    ///< idem
    std::vector<float> cr;    ///< idem
    std::vector<float> alpha; ///< width × height, coverage 0..1

    bool empty() const { return width <= 0 || height <= 0 || inkWidth <= 0; }
};

/// The two planes of a 4:2:0 picture as writable memory.
struct PlaneViews
{
    uint8_t* y = nullptr;
    size_t yStride = 0; ///< bytes per row
    uint8_t* uv = nullptr;
    size_t uvStride = 0; ///< bytes per row of the interleaved CbCr plane
    int width = 0;       ///< picture size in pixels (even)
    int height = 0;
    bool tenBit = false; ///< P010: 16-bit words, the code in the top 10 bits
};

/// Where the pointer goes: the top-left of the scaled shape in picture
/// pixels (may be negative, may overflow — the blend clips) and the picture
/// pixels per shape pixel (1 = the shape's own size).
struct CursorPlacement
{
    float x = 0.0f;
    float y = 0.0f;
    float scale = 1.0f;
};

/// A rectangle of the picture, in pixels; even-aligned so it covers whole
/// chroma samples.
struct BlendRect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool empty() const { return width <= 0 || height <= 0; }
};

/// What the blend overwrote, so the same buffer can be re-encoded with the
/// pointer somewhere else: the still-screen floor and the refinement burst
/// re-encode the compositor's LAST buffer, and a pointer burnt into it would
/// leave a trail. Save before, restore before the next blend.
struct PlanePatch
{
    BlendRect rect;
    std::vector<uint8_t> y;
    std::vector<uint8_t> uv;
    bool valid() const { return !rect.empty(); }
};

namespace detail {

inline float srgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

/// SMPTE ST 2084, normalised light (1.0 = 10 000 nits) → PQ signal 0..1.
inline float pqEncode(float light)
{
    if (light <= 0.0f) return 0.0f;
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 4096.0f * 128.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 4096.0f * 32.0f;
    constexpr float c3 = 2392.0f / 4096.0f * 32.0f;
    const float p = std::pow(light, m1);
    return std::pow((c1 + c2 * p) / (1.0f + c3 * p), m2);
}

} // namespace detail

/// Prepare a premultiplied sRGB BGRA shape for `target`.
inline PreparedCursor prepareCursor(const uint8_t* bgraPremultiplied, int width, int height,
                                    BlendTarget target)
{
    PreparedCursor out;
    if (!bgraPremultiplied || width <= 0 || height <= 0) return out;
    out.width = width;
    out.height = height;
    out.target = target;
    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    out.luma.assign(n, 0.0f);
    out.cb.assign(n, 0.0f);
    out.cr.assign(n, 0.0f);
    out.alpha.assign(n, 0.0f);

    int inkRight = -1, inkBottom = -1;
    for (int yy = 0; yy < height; ++yy) {
        for (int xx = 0; xx < width; ++xx) {
            const size_t i = static_cast<size_t>(yy) * width + xx;
            const uint8_t* p = bgraPremultiplied + i * 4;
            const float a = p[3] / 255.0f;
            if (a <= 0.0f) continue;
            if (xx > inkRight) inkRight = xx;
            if (yy > inkBottom) inkBottom = yy;
            out.alpha[i] = a;
            // Premultiplied colour, gamma-encoded, 0..1.
            const float bp = p[0] / 255.0f, gp = p[1] / 255.0f, rp = p[2] / 255.0f;

            if (target == BlendTarget::Nv12Bt709) {
                // Linear in the code values, so the premultiplied colour IS
                // the contribution and the offsets scale by coverage.
                const float l = 0.2126f * rp + 0.7152f * gp + 0.0722f * bp;
                const float cbv = -0.1146f * rp - 0.3854f * gp + 0.5f * bp;
                const float crv = 0.5f * rp - 0.4542f * gp - 0.0458f * bp;
                out.luma[i] = 16.0f * a + 219.0f * l;
                out.cb[i] = 128.0f * a + 224.0f * cbv;
                out.cr[i] = 128.0f * a + 224.0f * crv;
            } else {
                // PQ is not linear in anything: un-premultiply, go through
                // light, and premultiply the resulting code values.
                const float r = rp / a, g = gp / a, b = bp / a;
                const float rl = detail::srgbToLinear(r), gl = detail::srgbToLinear(g),
                            bl = detail::srgbToLinear(b);
                // BT.709 → BT.2020 primaries, in linear light.
                const float r2 = 0.6274f * rl + 0.3293f * gl + 0.0433f * bl;
                const float g2 = 0.0691f * rl + 0.9195f * gl + 0.0114f * bl;
                const float b2 = 0.0164f * rl + 0.0880f * gl + 0.8956f * bl;
                // SDR white at 203 nits (BT.2408), against PQ's 10 000.
                constexpr float kSdrWhite = 203.0f / 10000.0f;
                const float rq = detail::pqEncode(r2 * kSdrWhite);
                const float gq = detail::pqEncode(g2 * kSdrWhite);
                const float bq = detail::pqEncode(b2 * kSdrWhite);
                // BT.2020 non-constant luminance, on the PQ signal.
                const float yq = 0.2627f * rq + 0.6780f * gq + 0.0593f * bq;
                const float cbq = (bq - yq) / 1.8814f;
                const float crq = (rq - yq) / 1.4746f;
                out.luma[i] = a * (64.0f + 876.0f * yq);
                out.cb[i] = a * (512.0f + 896.0f * cbq);
                out.cr[i] = a * (512.0f + 896.0f * crq);
            }
        }
    }
    out.inkWidth = inkRight + 1;
    out.inkHeight = inkBottom + 1;
    return out;
}

namespace detail {

inline float sampleBilinear(const std::vector<float>& plane, int width, int height, float sx,
                            float sy)
{
    // Sample at pixel centres; outside the shape is transparent.
    sx -= 0.5f;
    sy -= 0.5f;
    const int x0 = static_cast<int>(std::floor(sx));
    const int y0 = static_cast<int>(std::floor(sy));
    const float fx = sx - x0, fy = sy - y0;
    auto at = [&](int x, int y) -> float {
        if (x < 0 || y < 0 || x >= width || y >= height) return 0.0f;
        return plane[static_cast<size_t>(y) * width + x];
    };
    const float top = at(x0, y0) * (1.0f - fx) + at(x0 + 1, y0) * fx;
    const float bottom = at(x0, y0 + 1) * (1.0f - fx) + at(x0 + 1, y0 + 1) * fx;
    return top * (1.0f - fy) + bottom * fy;
}

inline int readCode(const PlaneViews& planes, const uint8_t* row, int x)
{
    if (planes.tenBit) {
        uint16_t w;
        std::memcpy(&w, row + static_cast<size_t>(x) * 2, 2);
        return w >> 6;
    }
    return row[x];
}

inline void writeCode(const PlaneViews& planes, uint8_t* row, int x, float value, int lo, int hi)
{
    int code = static_cast<int>(value + 0.5f);
    if (code < lo) code = lo;
    if (code > hi) code = hi;
    if (planes.tenBit) {
        const uint16_t w = static_cast<uint16_t>(code << 6);
        std::memcpy(row + static_cast<size_t>(x) * 2, &w, 2);
    } else {
        row[x] = static_cast<uint8_t>(code);
    }
}

} // namespace detail

/// The picture rectangle a placement of `cursor` touches, clipped to the
/// picture and even-aligned. Empty when the pointer is entirely outside.
inline BlendRect blendFootprint(const PreparedCursor& cursor, const CursorPlacement& placement,
                                const PlaneViews& planes)
{
    BlendRect r;
    if (cursor.empty() || !(placement.scale > 0.0f)) return r;
    int x0 = static_cast<int>(std::floor(placement.x));
    int y0 = static_cast<int>(std::floor(placement.y));
    int x1 = static_cast<int>(std::ceil(placement.x + cursor.inkWidth * placement.scale));
    int y1 = static_cast<int>(std::ceil(placement.y + cursor.inkHeight * placement.scale));
    x0 &= ~1;
    y0 &= ~1;
    x1 = (x1 + 1) & ~1;
    y1 = (y1 + 1) & ~1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > planes.width) x1 = planes.width;
    if (y1 > planes.height) y1 = planes.height;
    if (x1 <= x0 || y1 <= y0) return r;
    r.x = x0;
    r.y = y0;
    r.width = x1 - x0;
    r.height = y1 - y0;
    return r;
}

/// Copy `rect` of both planes out of the picture.
inline void savePatch(const PlaneViews& planes, const BlendRect& rect, PlanePatch& patch)
{
    patch.rect = rect;
    patch.y.clear();
    patch.uv.clear();
    if (rect.empty()) return;
    const size_t bpp = planes.tenBit ? 2 : 1;
    const size_t rowBytes = static_cast<size_t>(rect.width) * bpp;
    patch.y.resize(rowBytes * rect.height);
    for (int row = 0; row < rect.height; ++row)
        std::memcpy(patch.y.data() + row * rowBytes,
                    planes.y + static_cast<size_t>(rect.y + row) * planes.yStride + rect.x * bpp,
                    rowBytes);
    const size_t uvRowBytes = static_cast<size_t>(rect.width) * bpp; // width/2 samples × 2
    const int uvRows = rect.height / 2;
    patch.uv.resize(uvRowBytes * uvRows);
    for (int row = 0; row < uvRows; ++row)
        std::memcpy(patch.uv.data() + row * uvRowBytes,
                    planes.uv + static_cast<size_t>(rect.y / 2 + row) * planes.uvStride +
                        rect.x * bpp,
                    uvRowBytes);
}

/// Put a saved rectangle back. The picture must be the one it was saved from.
inline void restorePatch(PlaneViews& planes, const PlanePatch& patch)
{
    const BlendRect& rect = patch.rect;
    if (rect.empty() || rect.x + rect.width > planes.width || rect.y + rect.height > planes.height)
        return;
    const size_t bpp = planes.tenBit ? 2 : 1;
    const size_t rowBytes = static_cast<size_t>(rect.width) * bpp;
    for (int row = 0; row < rect.height; ++row)
        std::memcpy(planes.y + static_cast<size_t>(rect.y + row) * planes.yStride + rect.x * bpp,
                    patch.y.data() + row * rowBytes, rowBytes);
    const size_t uvRowBytes = static_cast<size_t>(rect.width) * bpp;
    const int uvRows = rect.height / 2;
    for (int row = 0; row < uvRows; ++row)
        std::memcpy(planes.uv + static_cast<size_t>(rect.y / 2 + row) * planes.uvStride +
                        rect.x * bpp,
                    patch.uv.data() + row * uvRowBytes, uvRowBytes);
}

/// Draw `cursor` at `placement` into `planes`. Returns the rectangle touched
/// (what savePatch() should have been given beforehand).
inline BlendRect blendCursor(const PreparedCursor& cursor, const CursorPlacement& placement,
                             PlaneViews& planes)
{
    const BlendRect rect = blendFootprint(cursor, placement, planes);
    if (rect.empty()) return rect;
    const float inv = 1.0f / placement.scale;
    const int lumaLo = planes.tenBit ? 64 : 16;
    const int lumaHi = planes.tenBit ? 940 : 235;
    const int chromaLo = planes.tenBit ? 64 : 16;
    const int chromaHi = planes.tenBit ? 960 : 240;

    // Per picture pixel of the footprint: coverage and the three
    // contributions, sampled from the shape.
    const size_t n = static_cast<size_t>(rect.width) * rect.height;
    std::vector<float> a(n), yl(n), cb(n), cr(n);
    for (int row = 0; row < rect.height; ++row) {
        for (int col = 0; col < rect.width; ++col) {
            const float px = static_cast<float>(rect.x + col) + 0.5f;
            const float py = static_cast<float>(rect.y + row) + 0.5f;
            const float sx = (px - placement.x) * inv;
            const float sy = (py - placement.y) * inv;
            const size_t i = static_cast<size_t>(row) * rect.width + col;
            a[i] = detail::sampleBilinear(cursor.alpha, cursor.width, cursor.height, sx, sy);
            if (a[i] <= 0.0f) {
                a[i] = 0.0f;
                yl[i] = cb[i] = cr[i] = 0.0f;
                continue;
            }
            yl[i] = detail::sampleBilinear(cursor.luma, cursor.width, cursor.height, sx, sy);
            cb[i] = detail::sampleBilinear(cursor.cb, cursor.width, cursor.height, sx, sy);
            cr[i] = detail::sampleBilinear(cursor.cr, cursor.width, cursor.height, sx, sy);
        }
    }

    // Luma, every pixel.
    for (int row = 0; row < rect.height; ++row) {
        uint8_t* line = planes.y + static_cast<size_t>(rect.y + row) * planes.yStride;
        for (int col = 0; col < rect.width; ++col) {
            const size_t i = static_cast<size_t>(row) * rect.width + col;
            if (a[i] <= 0.0f) continue;
            const float dst = static_cast<float>(detail::readCode(planes, line, rect.x + col));
            detail::writeCode(planes, line, rect.x + col, dst * (1.0f - a[i]) + yl[i], lumaLo,
                              lumaHi);
        }
    }
    // Chroma, one sample per 2×2 block: the block's mean coverage and mean
    // contribution, so an edge that half-covers a block half-tints it.
    for (int row = 0; row < rect.height; row += 2) {
        uint8_t* line = planes.uv + static_cast<size_t>((rect.y + row) / 2) * planes.uvStride;
        for (int col = 0; col < rect.width; col += 2) {
            float ma = 0.0f, mcb = 0.0f, mcr = 0.0f;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const size_t i = static_cast<size_t>(row + dy) * rect.width + col + dx;
                    ma += a[i];
                    mcb += cb[i];
                    mcr += cr[i];
                }
            ma *= 0.25f;
            if (ma <= 0.0f) continue;
            mcb *= 0.25f;
            mcr *= 0.25f;
            const int sample = rect.x + col; // interleaved: Cb at sample, Cr at sample + 1
            const float dcb = static_cast<float>(detail::readCode(planes, line, sample));
            const float dcr = static_cast<float>(detail::readCode(planes, line, sample + 1));
            detail::writeCode(planes, line, sample, dcb * (1.0f - ma) + mcb, chromaLo, chromaHi);
            detail::writeCode(planes, line, sample + 1, dcr * (1.0f - ma) + mcr, chromaLo,
                              chromaHi);
        }
    }
    return rect;
}

} // namespace mw::native::convert
