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

#include "convert/MwsrZones.h"
#include "mwsr_zone_cases.h"
#include "mwsr_zone_cases_light.h"
#include "native_test_framework.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

namespace mz = mw::native::convert::mwsrzones;

void run_mwsr_zones_tests()
{
    SECTION("MwsrZones — the tree gives the bench's code on the bench's zones");
    {
        int same = 0;
        for (const MwsrZoneCase& c : kMwsrZoneCases)
            same += mz::decide(c.in) == c.code;
        CHECK_EQ(same, int(sizeof kMwsrZoneCases / sizeof kMwsrZoneCases[0]));
    }

    SECTION("MwsrZones — a zone from its cells, through the f16 roundings, gets the bench's code");
    {
        for (const MwsrZoneCells& c : kMwsrZoneCells) {
            // the zone alone in a grid: its 4 x 4 cells at zone (0, 0)
            std::vector<float> cells(size_t(mz::kCellsX) * mz::kCellsY * mz::kStats, 0.0f);
            for (int k = 0; k < 16; ++k)
                for (int s = 0; s < mz::kStats; ++s)
                    cells[(size_t(k / 4) * mz::kCellsX + k % 4) * mz::kStats + s] = c.cells[k][s];
            float m[mz::kStats], f[mz::kStats], in[mz::kInputs];
            mz::zoneMeans(cells.data(), 0, 0, m);
            mz::features(m, f);
            in[0] = c.scale;
            for (int s = 0; s < mz::kStats; ++s) {
                in[1 + s] = f[s];
                in[1 + mz::kStats + s] = c.frame[s];
            }
            in[17] = c.encoded;
            in[18] = c.linear;
            CHECK_EQ(mz::decide(in), c.code);
        }
    }

    SECTION("MwsrZones — the light tree gives the bench's code on the bench's light zones");
    {
        int same = 0;
        for (const MwsrZoneCase& c : kMwsrZoneCasesLight)
            same += mz::decide(c.in, mz::kTreeLight) == c.code;
        CHECK_EQ(same, int(sizeof kMwsrZoneCasesLight / sizeof kMwsrZoneCasesLight[0]));
    }

    SECTION("MwsrZones — a light zone from its 8 x 8 cells gets the bench's code");
    {
        for (const MwsrZoneCellsLight& c : kMwsrZoneCellsLight) {
            // the zone alone in a grid: its 8 x 8 cells at light zone (1, 1)
            std::vector<float> cells(size_t(mz::kCellsX) * mz::kCellsY * mz::kStats, 0.0f);
            for (int k = 0; k < 64; ++k)
                for (int s = 0; s < mz::kStats; ++s)
                    cells[(size_t(8 + k / 8) * mz::kCellsX + 8 + k % 8) * mz::kStats + s] =
                        c.cells[k][s];
            float m[mz::kStats], f[mz::kStats], in[mz::kInputs];
            mz::zoneMeans(cells.data(), 1, 1, m, mz::kLightGrid.cellsPerZone());
            mz::features(m, f);
            in[0] = c.scale;
            for (int s = 0; s < mz::kStats; ++s) {
                in[1 + s] = f[s];
                in[1 + mz::kStats + s] = c.frame[s];
            }
            in[17] = c.encoded;
            in[18] = c.linear;
            CHECK_EQ(mz::decide(in, mz::kTreeLight), c.code);
        }
    }

    SECTION("MwsrZones — the light grid: 16 x 9 zones of 8 x 8 cells, its own tree");
    {
        CHECK_EQ(mz::kLightGrid.zones(), 144);
        CHECK_EQ(mz::kLightGrid.cellsPerZone(), 8);
        CHECK_EQ(mz::kFullGrid.zones(), mz::kZones);
        CHECK_EQ(mz::kFullGrid.cellsPerZone(), 4);
        CHECK(mz::kLightGrid.tree == mz::kTreeLight);
        for (const mz::TreeNode& n : mz::kTreeLight)
            if (n.feature < 0)
                CHECK(n.code >= 0 && n.code < mz::kCodes);
            else
                CHECK(n.feature < mz::kInputs);
    }

    SECTION("MwsrZones — Bruno's budget: the full pass, else the light one, else none");
    {
        // 1 ms, tighter than the Lanczos-2's (31/10/2026); the full pass first
        const int64_t budget = mz::kBudgetUs;
        CHECK_EQ(budget, 1000);
        CHECK(budget < mw::native::convert::ResampleCost::kBudgetUs);
        CHECK(!mz::kStartLight);
        CHECK(mz::afterPassCost(false, 90) == mz::PassStep::Keep);
        CHECK(mz::afterPassCost(false, budget) == mz::PassStep::Keep);
        CHECK(mz::afterPassCost(false, budget + 1) == mz::PassStep::Lighten);
        CHECK(mz::afterPassCost(true, 740) == mz::PassStep::Keep);
        CHECK(mz::afterPassCost(true, budget + 1) == mz::PassStep::Drop);
        // no timing (a negative median): never kept
        CHECK(mz::afterPassCost(false, -1) == mz::PassStep::Lighten);
        CHECK(mz::afterPassCost(true, -1) == mz::PassStep::Drop);
    }

    SECTION("MwsrZones — f16 as numpy rounds it: to nearest, ties to even, subnormals");
    {
        CHECK_EQ(mz::roundToHalf(1.0f), 1.0f);
        CHECK_EQ(mz::roundToHalf(0.1f), 0.0999755859375f);
        // halfway between 1 and 1 + 2^-10: to the even one, 1
        CHECK_EQ(mz::roundToHalf(1.0f + 0.00048828125f), 1.0f);
        // halfway between 1 + 2^-10 and 1 + 2^-9: to the even one, 1 + 2^-9
        CHECK_EQ(mz::roundToHalf(1.0f + 3 * 0.00048828125f), 1.0f + 0.001953125f);
        // under the smallest normal half: a multiple of 2^-24
        CHECK_EQ(mz::roundToHalf(1e-6f), 17.0f / 16777216.0f);
        CHECK_EQ(mz::roundToHalf(-0.5f), -0.5f);
        CHECK_EQ(mz::roundToHalf(0.0f), 0.0f);
#if MW_MWSR_ZONES_SSE2
        // four at once: the same bits as one at a time — the edges of each
        // branch, then a run of patterns over all the floats
        const auto same = [](uint32_t bits) {
            float x;
            std::memcpy(&x, &bits, sizeof(x));
            const float one = mz::roundToHalf(x);
            float four[4];
            _mm_storeu_ps(four, mz::roundToHalf4(_mm_set1_ps(x)));
            uint32_t a, b;
            std::memcpy(&a, &one, sizeof(a));
            std::memcpy(&b, &four[3], sizeof(b));
            return a == b;
        };
        int wrong = 0;
        for (uint32_t edge :
             {0x00000000u, 0x00000001u, 0x007FFFFFu, 0x00800000u, 0x33000000u, 0x33000001u,
              0x337FFFFFu, 0x33800000u, 0x387FC000u, 0x387FE000u, 0x387FEFFFu, 0x387FF000u,
              0x387FFFFFu, 0x38800000u, 0x38801000u, 0x38803000u, 0x3F800FFFu, 0x3F801000u,
              0x3F803000u, 0x477FDFFFu, 0x477FE000u, 0x477FEFFFu, 0x477FF000u, 0x7F7FFFFFu,
              0x7F800000u, 0x7F800001u, 0x7FC00000u, 0x7FFFFFFFu})
            wrong += !same(edge) + !same(edge | 0x80000000u);
        uint32_t bits = 12345u;
        for (int i = 0; i < 1000000; ++i) {
            bits = bits * 1664525u + 1013904223u;
            wrong += !same(bits);
        }
        CHECK_EQ(wrong, 0);
#endif
    }

    SECTION("MwsrZones — a stream's memo: the codes without it, its zones read only where they "
            "changed");
    {
        // cells of every kind: zeros, means under the smallest normal half, the rest
        std::vector<float> cells(static_cast<size_t>(mz::kCellFloats));
        uint32_t seed = 2026u;
        const auto next = [&seed]() {
            seed = seed * 1664525u + 1013904223u;
            return seed;
        };
        for (float& c : cells) {
            const uint32_t r = next();
            c = (r & 7u) == 0   ? 0.0f
                : (r & 7u) == 1 ? float(r >> 8) * 1e-12f
                                : float(r >> 8) / 16777216.0f;
        }
        mz::ZoneMemo memo;
        int checked = 0, wrong = 0;
        const auto both = [&](const mz::Grid& grid, float scale) {
            uint8_t want[mz::kZones], got[mz::kZones];
            mz::decideAll(cells.data(), scale, true, want, grid);
            mz::decideAll(cells.data(), scale, true, got, grid, &memo);
            for (int z = 0; z < grid.zones(); ++z)
                wrong += want[z] != got[z];
            ++checked;
        };
        both(mz::kFullGrid, 2.0f);
        CHECK_EQ(memo.read, int64_t(mz::kZones)); // the first picture: every zone
        both(mz::kFullGrid, 2.0f);
        CHECK_EQ(memo.read, int64_t(mz::kZones)); // the same cells: none
        // one value of one cell, in the zone (5, 3): that zone alone
        cells[((size_t(3) * 4 + 2) * mz::kCellsX + 5 * 4 + 1) * mz::kStats + 6] += 0.25f;
        both(mz::kFullGrid, 4.0f / 3.0f);
        CHECK_EQ(memo.read, int64_t(mz::kZones) + 1);
        // pictures that change a few zones, then many
        for (int picture = 0; picture < 8; ++picture) {
            const int changes = picture < 4 ? 3 : 4000;
            for (int k = 0; k < changes; ++k)
                cells[next() % cells.size()] = float(next() >> 8) / 16777216.0f;
            both(mz::kFullGrid, picture % 2 ? 2.0f : 4.0f / 3.0f);
        }
        // the light grid: every zone again, its own codes
        const int64_t before = memo.read;
        both(mz::kLightGrid, 2.0f);
        CHECK_EQ(memo.read, before + mz::kLightGrid.zones());
        both(mz::kFullGrid, 2.0f); // and back
        CHECK_EQ(checked, 13);
        CHECK_EQ(wrong, 0);
        CHECK_EQ(memo.zones, int64_t(12) * mz::kZones + mz::kLightGrid.zones());
    }

    SECTION("MwsrZones — a code is a drop under the law, never above it");
    {
        CHECK_EQ(mz::dropOf(0), 0.0f);
        CHECK_EQ(mz::dropOf(1), 0.1f);
        CHECK(std::fabs(mz::dropOf(11) - 1.35f) < 1e-6f);
        for (const mz::TreeNode& n : mz::kTree)
            if (n.feature < 0) CHECK(n.code >= 0 && n.code < mz::kCodes);
    }

    SECTION("MwsrZones — a flat grey frame keeps the law in every zone, at x2 and x1.33");
    {
        // luma 0.5 everywhere: no gradient, every pixel flat, nothing dark or saturated
        std::vector<float> cells(size_t(mz::kCellsX) * mz::kCellsY * mz::kStats, 0.0f);
        for (size_t i = 0; i < cells.size(); i += mz::kStats) {
            cells[i + 0] = 0.5f;
            cells[i + 1] = 0.25f;
            cells[i + 4] = 1.0f;
        }
        for (float scale : {2.0f, 4.0f / 3.0f}) {
            uint8_t codes[mz::kZones];
            mz::decideAll(cells.data(), scale, true, codes);
            int law = 0;
            for (uint8_t c : codes)
                law += c == 0;
            CHECK_EQ(law, mz::kZones);
            // the light grid too, and it writes its 144 codes only
            uint8_t light[mz::kZones];
            std::fill(std::begin(light), std::end(light), uint8_t(0xEE));
            mz::decideAll(cells.data(), scale, true, light, mz::kLightGrid);
            int lightLaw = 0, untouched = 0;
            for (int z = 0; z < mz::kZones; ++z) {
                if (z < mz::kLightGrid.zones())
                    lightLaw += light[z] == 0;
                else
                    untouched += light[z] == 0xEE;
            }
            CHECK_EQ(lightLaw, mz::kLightGrid.zones());
            CHECK_EQ(untouched, mz::kZones - mz::kLightGrid.zones());
        }
    }

    SECTION("MwsrZones — the guard: a zone drops no more than the least of its neighbours");
    {
        // a field at code 5, one zone at the law: its 3 x 3 neighbourhood takes the law
        uint8_t codes[mz::kZones];
        std::fill(std::begin(codes), std::end(codes), uint8_t(5));
        codes[4 * mz::kZonesX + 7] = 0;
        mz::guardNeighbours(codes, mz::kZonesX, mz::kZonesY);
        int law = 0;
        for (int zy = 0; zy < mz::kZonesY; ++zy)
            for (int zx = 0; zx < mz::kZonesX; ++zx) {
                const bool near = zy >= 3 && zy <= 5 && zx >= 6 && zx <= 8;
                CHECK_EQ(int(codes[zy * mz::kZonesX + zx]), near ? 0 : 5);
                law += codes[zy * mz::kZonesX + zx] == 0;
            }
        CHECK_EQ(law, 9);

        // the edges clamped: a corner has 4 zones around it, an edge 6
        std::fill(std::begin(codes), std::end(codes), uint8_t(8));
        codes[0] = 3;
        codes[(mz::kZonesY - 1) * mz::kZonesX + 10] = 1;
        mz::guardNeighbours(codes, mz::kZonesX, mz::kZonesY);
        CHECK_EQ(int(codes[0]), 3);
        CHECK_EQ(int(codes[1]), 3);
        CHECK_EQ(int(codes[mz::kZonesX]), 3);
        CHECK_EQ(int(codes[mz::kZonesX + 1]), 3);
        CHECK_EQ(int(codes[2]), 8);
        CHECK_EQ(int(codes[2 * mz::kZonesX]), 8);
        const int last = (mz::kZonesY - 1) * mz::kZonesX;
        for (int x = 9; x <= 11; ++x) {
            CHECK_EQ(int(codes[last + x]), 1);
            CHECK_EQ(int(codes[last - mz::kZonesX + x]), 1);
        }
        CHECK_EQ(int(codes[last + 12]), 8);
        CHECK_EQ(int(codes[last - 2 * mz::kZonesX + 10]), 8);

        // two drops side by side: each takes the smaller where they meet, never a larger one
        std::fill(std::begin(codes), std::end(codes), uint8_t(9));
        for (int zy = 0; zy < mz::kZonesY; ++zy)
            for (int zx = 0; zx < mz::kZonesX / 2; ++zx)
                codes[zy * mz::kZonesX + zx] = 2;
        mz::guardNeighbours(codes, mz::kZonesX, mz::kZonesY);
        CHECK_EQ(int(codes[mz::kZonesX / 2 - 1]), 2);
        CHECK_EQ(int(codes[mz::kZonesX / 2]), 2);
        CHECK_EQ(int(codes[mz::kZonesX / 2 + 1]), 9);

        // the light grid's 16 x 9: only its 144 codes are read and written
        uint8_t light[mz::kZones];
        std::fill(std::begin(light), std::end(light), uint8_t(0xEE));
        for (int z = 0; z < mz::kLightGrid.zones(); ++z)
            light[z] = 4;
        light[mz::kLightGrid.zones() - 1] = 0; // the last corner at the law
        mz::guardNeighbours(light, mz::kLightZonesX, mz::kLightZonesY);
        int lightLaw = 0, untouched = 0;
        for (int z = 0; z < mz::kZones; ++z) {
            if (z < mz::kLightGrid.zones())
                lightLaw += light[z] == 0;
            else
                untouched += light[z] == 0xEE;
        }
        CHECK_EQ(lightLaw, 4);
        CHECK_EQ(untouched, mz::kZones - mz::kLightGrid.zones());
    }
}
