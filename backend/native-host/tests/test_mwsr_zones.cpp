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
#include "native_test_framework.h"

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
        }
    }
}
