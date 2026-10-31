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
#include "convert/MwsrZoneTreeLight.h"
#include "convert/ResampleCost.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

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
// ── The light grid ──────────────────────────────────────────────────────────
// Bruno's budget (§12 f.4, 30/10/2026): where the full pass costs too much,
// the light one — one pixel in 16 into the same cells, and a 16 x 9 grid of
// 8 x 8 cells per zone, decided by its own tree (kTreeLight), as the bench
// measured it. The client reads a grid of any size.
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
constexpr int kLightZonesX = 16;
constexpr int kLightZonesY = 9;

/// A grid of zones over the cells, and the tree that decides its zones.
struct Grid
{
    int zonesX;
    int zonesY;
    const TreeNode* tree;

    constexpr int zones() const { return zonesX * zonesY; }
    /// Cells along a zone's side: as many across as down.
    constexpr int cellsPerZone() const { return kCellsX / zonesX; }
};

inline constexpr Grid kFullGrid = {kZonesX, kZonesY, kTree};
inline constexpr Grid kLightGrid = {kLightZonesX, kLightZonesY, kTreeLight};

/// What the statistics become once their pass's cost on this GPU is in.
enum class PassStep
{
    Keep,    ///< within the budget
    Lighten, ///< the full pass costs too much: the light one, timed in turn
    Drop,    ///< the light one too: no grid, the client keeps its law
};

/// Bruno's budget (§12 f.4, 30/10/2026): the Lanczos-2's (ResampleCost), the
/// full pass where it fits, else the light one, else nothing.
inline PassStep afterPassCost(bool light, int64_t costUs)
{
    if (ResampleCost::affordable(costUs)) return PassStep::Keep;
    return light ? PassStep::Drop : PassStep::Lighten;
}

static_assert(kFullGrid.cellsPerZone() * kZonesX == kCellsX &&
                  kFullGrid.cellsPerZone() * kZonesY == kCellsY,
              "whole cells per zone");
static_assert(kLightGrid.cellsPerZone() * kLightZonesX == kCellsX &&
                  kLightGrid.cellsPerZone() * kLightZonesY == kCellsY,
              "whole cells per zone");

/// @p x rounded to IEEE binary16 and back, to nearest, ties to even, as numpy's
/// float16 (subnormals included; past the largest half, infinity).
///
/// On the float's bits: a decision rounds 78 336 values, on the thread that
/// sends the frame, and through frexp and ldexp it took 0.30 ms a frame on a
/// Ryzen 9 9900X, 0.09 this way. The same result as theirs on every one of the
/// 2^32 floats (checked exhaustively, 30/10/2026).
inline float roundToHalf(float x)
{
    uint32_t u;
    std::memcpy(&u, &x, sizeof(u));
    const uint32_t sign = u & 0x80000000u;
    uint32_t a = u & 0x7FFFFFFFu;
    if (a > 0x7F800000u) return x; // NaN
    if (a >= 0x477FF000u) {
        // 65520 and past: infinity
        a = 0x7F800000u;
    } else if (a >= 0x38800000u) {
        // a normal half, 2^-14 and up: 10 of the 23 bits kept, to nearest even
        // (a carry runs into the exponent, as it should)
        a += 0x0FFFu + ((a >> 13) & 1u);
        a &= ~0x1FFFu;
    } else {
        // a subnormal half: a multiple of 2^-24, both scalings exact
        float f;
        std::memcpy(&f, &a, sizeof(f));
        f = std::nearbyint(f * 16777216.0f) * (1.0f / 16777216.0f);
        std::memcpy(&a, &f, sizeof(a));
    }
    u = sign | a;
    float r;
    std::memcpy(&r, &u, sizeof(r));
    return r;
}

/// A zone's 8 means from its @p per x @p per cells — 4 on the full grid, 8 on
/// the light one (@p cells: the 128 x 72 grid, row by row, 8 floats per cell,
/// as the pass wrote them): the bench's tiles().
inline void zoneMeans(const float* cells, int zx, int zy, float out[kStats],
                      int per = kFullGrid.cellsPerZone())
{
    float acc[kStats] = {};
    for (int y = 0; y < per; ++y) {
        for (int x = 0; x < per; ++x) {
            const float* c =
                cells + ((size_t(zy) * per + y) * kCellsX + (size_t(zx) * per + x)) * kStats;
            for (int s = 0; s < kStats; ++s)
                acc[s] = static_cast<float>(static_cast<double>(acc[s]) + roundToHalf(c[s]));
        }
    }
    for (int s = 0; s < kStats; ++s)
        out[s] = roundToHalf(static_cast<float>(static_cast<double>(acc[s]) / (per * per)));
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
inline int decide(const float in[kInputs], const TreeNode* tree = kTree)
{
    int i = 0;
    while (tree[i].feature >= 0)
        i = double(in[tree[i].feature]) <= tree[i].threshold ? tree[i].left : tree[i].right;
    return tree[i].code;
}

/// What the client takes off its sharpness law for @p code, as the across
/// weight (sharpness x the kernel's strength at its scale).
inline float dropOf(int code)
{
    return code <= 0 ? 0.0f : 0.1f + float(code - 1) * 0.125f;
}

/// Every zone's code of @p grid from the pass's cells (@p cells: 128 x 72 x 8
/// floats; the light grid's from the light pass's), row by row into @p codes
/// (grid.zones() of them, at most kZones). @p scale is the stream's upscale
/// at the client (the capture's width over the stream's); @p linear says the
/// host reduced in linear light (Lanczos-2), not with the bilinear of its
/// fallback.
inline void decideAll(const float* cells, float scale, bool linear, uint8_t* codes,
                      const Grid& grid = kFullGrid)
{
    const int zones = grid.zones();
    float feat[kZones][kStats];
    double sum[kStats] = {};
    for (int zy = 0; zy < grid.zonesY; ++zy) {
        for (int zx = 0; zx < grid.zonesX; ++zx) {
            float m[kStats];
            zoneMeans(cells, zx, zy, m, grid.cellsPerZone());
            float* f = feat[zy * grid.zonesX + zx];
            features(m, f);
            for (int s = 0; s < kStats; ++s)
                sum[s] += f[s];
        }
    }
    float in[kInputs];
    in[0] = scale;
    for (int s = 0; s < kStats; ++s)
        in[1 + kStats + s] = static_cast<float>(sum[s] / zones);
    in[17] = 1.0f; // encoded: a stream always is
    in[18] = linear ? 1.0f : 0.0f;
    for (int z = 0; z < zones; ++z) {
        for (int s = 0; s < kStats; ++s)
            in[1 + s] = feat[z][s];
        codes[z] = static_cast<uint8_t>(decide(in, grid.tree));
    }
}

} // namespace mw::native::convert::mwsrzones
