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

#include "convert/MwsrZoneTree.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

// MWSR's sharpness by zone, the native host's half (plan B, upscaler-mwsr-b.md
// §12 c, Bruno's decision of 30/10/2026): from the statistics of the native
// capture, one code per zone of a 32 x 18 grid, the drop the client's MWSR
// takes under its sharpness law there. The law is a ceiling: a code only ever
// lowers the sharpness, and code 0 is the law itself.
//
// ── What the statistics are ─────────────────────────────────────────────────
// Per pixel of the capture, on its sRGB-encoded values, its neighbours 1 px
// away and the picture's edges clamped: the luma (BT.709 weights) and its
// square, the gradient |dx| + |dy| (forward differences), whether it is above
// 0.05 and whether it is under 0.01, the |laplacian| (4 neighbours), the
// saturation (max - min of R, G, B) and whether the luma is under 0.15. Their
// means per cell of a 128 x 72 grid over the capture are the pass's output.
//
// ── Why the f16 roundings ───────────────────────────────────────────────────
// The bench kept the cells' means in f16 and the zones' (4 x 4 cells) in f16
// too, as its GPU pass did. The tree reads the luma's standard deviation from
// them, with thresholds as low as 0.00015: on means rounded otherwise, a flat
// zone's deviation moves across them. So the cells are rounded to f16 here,
// summed in order in float, and the zone's mean rounded to f16 again — exactly
// the bench's tiles(). Every float operation after that is the one numpy did in
// float32, taken through double and rounded back, so that no compiler can fuse
// two of them into one rounding.
//
// What is not measured (§12 g): a capture of another size than 2560 x 1440
// (the bench's), HDR (no grid then), a stream encoded at another quality than
// x264's QP 32 (the encoded flag is always set: a stream is always encoded).

namespace mw::native::convert::mwsrzones {

constexpr int kCellsX = 128;
constexpr int kCellsY = 72;
constexpr int kZonesX = 32;
constexpr int kZonesY = 18;
constexpr int kZones = kZonesX * kZonesY;
constexpr int kStats = 8;
constexpr int kInputs = 19;
/// Codes 0..11: 0 the law, k a drop of 0.1 + (k - 1) * 0.125.
constexpr int kCodes = 12;

/// @p x rounded to IEEE binary16 and back, to nearest, ties to even, as numpy's
/// float16 (subnormals included; past the largest half, infinity).
inline float roundToHalf(float x)
{
    if (std::isnan(x)) return x;
    const float a = std::fabs(x);
    if (a >= 65520.0f) return std::copysign(INFINITY, x);
    float q;
    if (a < 6.103515625e-05f) {
        // a subnormal half: a multiple of 2^-24
        q = static_cast<float>(std::nearbyint(static_cast<double>(a) * 16777216.0) / 16777216.0);
    } else {
        int e = 0;
        std::frexp(a, &e); // a = m * 2^e, m in [0.5, 1): 11 significant bits
        const double ulp = std::ldexp(1.0, e - 11);
        q = static_cast<float>(std::nearbyint(static_cast<double>(a) / ulp) * ulp);
    }
    return std::copysign(q, x);
}

/// A zone's 8 means from its 4 x 4 cells (@p cells: the 128 x 72 grid, row by
/// row, 8 floats per cell, as the pass wrote them): the bench's tiles().
inline void zoneMeans(const float* cells, int zx, int zy, float out[kStats])
{
    constexpr int kPer = kCellsX / kZonesX;
    float acc[kStats] = {};
    for (int y = 0; y < kPer; ++y) {
        for (int x = 0; x < kPer; ++x) {
            const float* c =
                cells + ((size_t(zy) * kPer + y) * kCellsX + (size_t(zx) * kPer + x)) * kStats;
            for (int s = 0; s < kStats; ++s)
                acc[s] = static_cast<float>(static_cast<double>(acc[s]) + roundToHalf(c[s]));
        }
    }
    for (int s = 0; s < kStats; ++s)
        out[s] = roundToHalf(static_cast<float>(static_cast<double>(acc[s]) / (kPer * kPer)));
}

/// The 8 features of a zone from its means: the luma, its standard deviation,
/// the gradient, the shares of edges and of flat pixels, the laplacian against
/// the gradient, the saturation, the share of dark pixels (the bench's
/// tile_feats, float32 operation by operation).
inline void features(const float m[kStats], float f[kStats])
{
    const auto r = [](double v) { return static_cast<float>(v); };
    const float square = r(double(m[0]) * m[0]);
    const float variance = r(double(m[1]) - square);
    f[0] = m[0];
    f[1] = r(std::sqrt(double(variance > 0.0f ? variance : 0.0f)));
    f[2] = m[2];
    f[3] = m[3];
    f[4] = m[4];
    f[5] = r(double(m[5]) / r(double(m[2]) + double(1e-3f)));
    f[6] = m[6];
    f[7] = m[7];
}

/// The tree's walk (MwsrZoneTree.h): a zone's 19 inputs -> its code.
inline int decide(const float in[kInputs])
{
    int i = 0;
    while (kTree[i].feature >= 0)
        i = double(in[kTree[i].feature]) <= kTree[i].threshold ? kTree[i].left : kTree[i].right;
    return kTree[i].code;
}

/// What the client takes off its sharpness law for @p code, as the across
/// weight (sharpness x the kernel's strength at its scale).
inline float dropOf(int code)
{
    return code <= 0 ? 0.0f : 0.1f + float(code - 1) * 0.125f;
}

/// Every zone's code from the pass's cells (@p cells: 128 x 72 x 8 floats).
/// @p scale is the stream's upscale at the client (the capture's width over
/// the stream's); @p linear says the host reduced in linear light (Lanczos-2),
/// not with the bilinear of its fallback.
inline void decideAll(const float* cells, float scale, bool linear, uint8_t codes[kZones])
{
    static_assert(kCellsX % kZonesX == 0 && kCellsY % kZonesY == 0, "whole cells per zone");
    float feat[kZones][kStats];
    double sum[kStats] = {};
    for (int zy = 0; zy < kZonesY; ++zy) {
        for (int zx = 0; zx < kZonesX; ++zx) {
            float m[kStats];
            zoneMeans(cells, zx, zy, m);
            float* f = feat[zy * kZonesX + zx];
            features(m, f);
            for (int s = 0; s < kStats; ++s)
                sum[s] += f[s];
        }
    }
    float in[kInputs];
    in[0] = scale;
    for (int s = 0; s < kStats; ++s)
        in[1 + kStats + s] = static_cast<float>(sum[s] / kZones);
    in[17] = 1.0f; // encoded: a stream always is
    in[18] = linear ? 1.0f : 0.0f;
    for (int z = 0; z < kZones; ++z) {
        for (int s = 0; s < kStats; ++s)
            in[1 + s] = feat[z][s];
        codes[z] = static_cast<uint8_t>(decide(in));
    }
}

} // namespace mw::native::convert::mwsrzones
