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
#include <vector>

#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#define MW_MWSR_ZONES_SSE2 1
#endif

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
// The light pass — one pixel in 16 into the same cells, and a 16 x 9 grid of
// 8 x 8 cells per zone, decided by its own tree (kTreeLight), as the bench
// measured it: where the full pass costs more than the budget (kBudgetUs), the
// light one, timed in turn (§12 f.4). The client reads a grid of any size.
//
// ── The guard ───────────────────────────────────────────────────────────────
// After the tree, a zone drops no more than the least of its neighbours
// (guardNeighbours): text beside a photo keeps the law (31/10/2026).
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

/// Bruno's budget for the zones (31/10/2026, after his eye test): 1 ms a
/// picture, timed in the stream — tighter than the Lanczos-2's 1.5
/// (ResampleCost), which was the zones' too from 30/10. Over it, no grid and
/// the client's law.
inline constexpr int64_t kBudgetUs = 1000;

/// The pass a stream starts on: the full one (Bruno's choice, 31/10/2026, after
/// the pumping test: in a real stream the full pass cost 64 us on an RTX, the
/// light one 35, both far under the budget, and the full one's 80-px zones
/// keep text and photos apart better). The light one only where the full one
/// is over the budget.
inline constexpr bool kStartLight = false;

/// The full pass where it fits the budget, else the light one, else nothing.
inline PassStep afterPassCost(bool light, int64_t costUs)
{
    if (costUs >= 0 && costUs <= kBudgetUs) return PassStep::Keep;
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

#if MW_MWSR_ZONES_SSE2
/// roundToHalf on four floats at once, its branches as masks: the same bits on
/// every float (checked exhaustively against it, 31/10/2026). SSE2 is part of
/// x64, so no CPU is asked.
inline __m128 roundToHalf4(__m128 x)
{
    const __m128i u = _mm_castps_si128(x);
    const __m128i sign = _mm_and_si128(u, _mm_set1_epi32(int(0x80000000u)));
    const __m128i a = _mm_and_si128(u, _mm_set1_epi32(0x7FFFFFFF));
    // a is under 2^31: the signed compares order it as the unsigned ones do
    const __m128i nan = _mm_cmpgt_epi32(a, _mm_set1_epi32(0x7F800000));
    const __m128i inf = _mm_cmpgt_epi32(a, _mm_set1_epi32(0x477FEFFF));
    const __m128i normal = _mm_cmpgt_epi32(a, _mm_set1_epi32(0x387FFFFF));
    // a normal half: 10 of the 23 bits kept, to nearest even
    const __m128i odd = _mm_and_si128(_mm_srli_epi32(a, 13), _mm_set1_epi32(1));
    const __m128i kept = _mm_andnot_si128(
        _mm_set1_epi32(0x1FFF), _mm_add_epi32(_mm_add_epi32(a, _mm_set1_epi32(0x0FFF)), odd));
    // a subnormal half: to the nearest multiple of 2^-24, even on a tie (2^23
    // added and taken off rounds what is under 1024 to an integer); the other
    // lanes go through as zero
    const __m128 under = _mm_andnot_ps(_mm_castsi128_ps(normal), _mm_castsi128_ps(a));
    const __m128 magic = _mm_set1_ps(8388608.0f);
    const __m128 steps =
        _mm_sub_ps(_mm_add_ps(_mm_mul_ps(under, _mm_set1_ps(16777216.0f)), magic), magic);
    __m128i r = _mm_castps_si128(_mm_mul_ps(steps, _mm_set1_ps(1.0f / 16777216.0f)));
    r = _mm_or_si128(_mm_and_si128(normal, kept), _mm_andnot_si128(normal, r));
    r = _mm_or_si128(_mm_and_si128(inf, _mm_set1_epi32(0x7F800000)), _mm_andnot_si128(inf, r));
    r = _mm_or_si128(_mm_and_si128(nan, a), _mm_andnot_si128(nan, r));
    return _mm_castsi128_ps(_mm_or_si128(sign, r));
}
#endif

/// A zone's 8 means from its @p per x @p per cells — 4 on the full grid, 8 on
/// the light one (@p cells: the 128 x 72 grid, row by row, 8 floats per cell,
/// as the pass wrote them): the bench's tiles().
inline void zoneMeans(const float* cells, int zx, int zy, float out[kStats],
                      int per = kFullGrid.cellsPerZone())
{
    float acc[kStats] = {};
#if MW_MWSR_ZONES_SSE2
    // The 8 statistics of a cell in two registers, each summed in the cells'
    // order. A float sum is the double one rounded back: rounding twice, to
    // 53 bits then to 24, gives the once-rounded sum (53 >= 2 x 24 + 2).
    static_assert(kStats == 8, "two registers a cell");
    __m128 lo = _mm_setzero_ps();
    __m128 hi = _mm_setzero_ps();
    for (int y = 0; y < per; ++y) {
        const float* c = cells + ((size_t(zy) * per + y) * kCellsX + size_t(zx) * per) * kStats;
        for (int x = 0; x < per; ++x, c += kStats) {
            lo = _mm_add_ps(lo, roundToHalf4(_mm_loadu_ps(c)));
            hi = _mm_add_ps(hi, roundToHalf4(_mm_loadu_ps(c + 4)));
        }
    }
    _mm_storeu_ps(acc, lo);
    _mm_storeu_ps(acc + 4, hi);
#else
    for (int y = 0; y < per; ++y) {
        for (int x = 0; x < per; ++x) {
            const float* c =
                cells + ((size_t(zy) * per + y) * kCellsX + (size_t(zx) * per + x)) * kStats;
            for (int s = 0; s < kStats; ++s)
                acc[s] = static_cast<float>(static_cast<double>(acc[s]) + roundToHalf(c[s]));
        }
    }
#endif
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

/// The guard for text beside photos (Bruno's choice, 31/10/2026): a zone drops
/// no more than the least of its 3 x 3 neighbourhood, itself included, the
/// grid's edges clamped. A zone the tree left at the law keeps its neighbours
/// at the law too: the client reads the grid bilinearly, so a photo's drop
/// spilt onto the caption beside it (the bench: -0.06 to -0.15 SSIM on text
/// beside photos at x2, none with the guard; the games keep most of their gain).
/// Codes grow with the drop, so the least drop is the smallest code.
inline void guardNeighbours(uint8_t* codes, int zonesX, int zonesY)
{
    uint8_t in[kZones];
    std::memcpy(in, codes, size_t(zonesX) * zonesY);
    for (int zy = 0; zy < zonesY; ++zy) {
        for (int zx = 0; zx < zonesX; ++zx) {
            uint8_t least = in[zy * zonesX + zx];
            for (int y = zy > 0 ? zy - 1 : 0; y <= (zy + 1 < zonesY ? zy + 1 : zy); ++y)
                for (int x = zx > 0 ? zx - 1 : 0; x <= (zx + 1 < zonesX ? zx + 1 : zx); ++x)
                    if (in[y * zonesX + x] < least) least = in[y * zonesX + x];
            codes[zy * zonesX + zx] = least;
        }
    }
}

/// The floats of the pass's cells: kCellsX x kCellsY of them, kStats each.
constexpr int kCellFloats = kCellsX * kCellsY * kStats;

/// What decideAll keeps of a stream's last picture (31/10/2026): its cells and
/// its zones' features, so that a zone whose cells have not changed by a bit
/// is not read again — a still desktop, a game's HUD. The same cells give the
/// same features: the codes are those decideAll gives without it.
struct ZoneMemo
{
    /// The last picture's cells, kCellFloats.
    std::vector<float> cells;
    /// Its zones' features, kStats each, of the grid of `tree`.
    std::vector<float> features;
    /// The tree of the grid they are of; null until the first decision.
    const TreeNode* tree = nullptr;
    /// Zones decided, and of them those read again: for the session's log.
    int64_t zones = 0;
    int64_t read = 0;
};

/// Every zone's code of @p grid from the pass's cells (@p cells: 128 x 72 x 8
/// floats; the light grid's from the light pass's), row by row into @p codes
/// (grid.zones() of them, at most kZones). @p scale is the stream's upscale
/// at the client (the capture's width over the stream's); @p linear says the
/// host reduced in linear light (Lanczos-2), not with the bilinear of its
/// fallback. The tree's codes, then the neighbours' guard (guardNeighbours).
/// With @p memo, a stream's: only the zones whose cells changed are read.
inline void decideAll(const float* cells, float scale, bool linear, uint8_t* codes,
                      const Grid& grid = kFullGrid, ZoneMemo* memo = nullptr)
{
    const int zones = grid.zones();
    const int per = grid.cellsPerZone();
    // a zone's row of cells, in bytes
    const size_t rowBytes = size_t(per) * kStats * sizeof(float);
    float local[kZones][kStats];
    float (*feat)[kStats] = local;
    bool all = true;
    if (memo) {
        if (memo->cells.size() != size_t(kCellFloats)) {
            memo->cells.assign(kCellFloats, 0.0f);
            memo->features.assign(size_t(kZones) * kStats, 0.0f);
            memo->tree = nullptr;
        }
        feat = reinterpret_cast<float (*)[kStats]>(memo->features.data());
        all = memo->tree != grid.tree;
        memo->tree = grid.tree;
        memo->zones += zones;
    }
    double sum[kStats] = {};
    for (int zy = 0; zy < grid.zonesY; ++zy) {
        for (int zx = 0; zx < grid.zonesX; ++zx) {
            const size_t first = ((size_t(zy) * per) * kCellsX + size_t(zx) * per) * kStats;
            bool changed = all;
            for (int y = 0; y < per && !changed; ++y) {
                const size_t at = first + size_t(y) * kCellsX * kStats;
                changed = std::memcmp(cells + at, memo->cells.data() + at, rowBytes) != 0;
            }
            float* f = feat[zy * grid.zonesX + zx];
            if (changed) {
                float m[kStats];
                zoneMeans(cells, zx, zy, m, per);
                features(m, f);
                if (memo) {
                    for (int y = 0; y < per; ++y) {
                        const size_t at = first + size_t(y) * kCellsX * kStats;
                        std::memcpy(memo->cells.data() + at, cells + at, rowBytes);
                    }
                    ++memo->read;
                }
            }
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
    guardNeighbours(codes, grid.zonesX, grid.zonesY);
}

} // namespace mw::native::convert::mwsrzones
