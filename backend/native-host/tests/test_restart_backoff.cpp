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

#include "core/RestartBackoff.h"
#include "native_test_framework.h"

using mw::native::restartRetryDelayMs;

void run_restart_backoff_tests()
{
    SECTION("RestartBackoff — quick first, then doubling to a one-second ceiling");
    {
        CHECK_EQ(restartRetryDelayMs(1), int64_t(100));
        CHECK_EQ(restartRetryDelayMs(2), int64_t(200));
        CHECK_EQ(restartRetryDelayMs(3), int64_t(400));
        CHECK_EQ(restartRetryDelayMs(4), int64_t(800));
        CHECK_EQ(restartRetryDelayMs(5), int64_t(1000));
        CHECK_EQ(restartRetryDelayMs(6), int64_t(1000));
        // A screen locked for an hour: still one attempt a second, no overflow.
        CHECK_EQ(restartRetryDelayMs(3600), int64_t(1000));
    }

    SECTION("RestartBackoff — a count that makes no sense reads as the first attempt");
    {
        CHECK_EQ(restartRetryDelayMs(0), int64_t(100));
        CHECK_EQ(restartRetryDelayMs(-3), int64_t(100));
    }

    SECTION("RestartBackoff — a mode set is covered inside the first second and a half");
    {
        // 100 + 200 + 400 + 800 = 1.5 s of retries before the ceiling: a
        // "Keep these display settings?" mode set is over well before that.
        int64_t total = 0;
        for (int attempt = 1; attempt <= 4; ++attempt)
            total += restartRetryDelayMs(attempt);
        CHECK_EQ(total, int64_t(1500));
    }
}
