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
        const int64_t budget = mw::native::convert::ResampleCost::kBudgetUs;
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
}
