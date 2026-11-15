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

#include "native_test_framework.h"

#include "capture/linux/TurnWatch.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace mw::native::capture;

// The turn watch of a KMS capture (TurnWatch.h): the compositor asked on a
// thread of its own, its word handed over as a flag. A stand-in reader plays
// the compositor; std::thread only, so every platform runs it.

namespace {

using Clock = std::chrono::steady_clock;

/// A compositor whose answer the test sets: a transform, or no answer.
struct FakeCompositor
{
    std::atomic<int> turns{0};
    std::atomic<bool> answers{true};
    std::atomic<int> blockMs{0};

    TurnWatch::Reader reader(const std::shared_ptr<FakeCompositor>& self)
    {
        return [self] {
            const int block = self->blockMs.load();
            if (block > 0) std::this_thread::sleep_for(std::chrono::milliseconds(block));
            CompositorTransform t;
            t.known = self->answers.load();
            t.orientation = Orientation{self->turns.load(), false};
            t.source = "wl_output test";
            return t;
        };
    }
};

/// take() asked as the capture loop would, until it answers or @p budgetMs.
bool takeWithin(TurnWatch& watch, CompositorTransform& seen, int budgetMs)
{
    const auto until = Clock::now() + std::chrono::milliseconds(budgetMs);
    while (Clock::now() < until) {
        if (watch.take(seen)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

} // namespace

void run_turn_watch_tests()
{
    SECTION("TurnWatch — nothing said while the display stays as it was");
    {
        auto fake = std::make_shared<FakeCompositor>();
        fake->turns = 1;
        TurnWatch watch(fake->reader(fake), Orientation{1, false}, 5);
        CompositorTransform seen;
        // Asked as the capture loop would until the thread has read at least
        // three times: counted by readings, not by a time that a loaded runner
        // can stretch (macOS CI, 15/11/2026: fewer than three in 60 ms).
        bool said = false;
        const auto until = Clock::now() + std::chrono::seconds(5);
        while (watch.readings() < 3 && Clock::now() < until) {
            if (watch.take(seen)) said = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(watch.readings() >= 3);
        CHECK(!said);
        CHECK(!watch.take(seen));
    }

    SECTION("TurnWatch — a turn is seen within a few intervals, and said once");
    {
        auto fake = std::make_shared<FakeCompositor>();
        TurnWatch watch(fake->reader(fake), Orientation{}, 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        fake->turns = 1;
        CompositorTransform seen;
        CHECK(takeWithin(watch, seen, 500));
        CHECK(seen.known);
        CHECK_EQ(seen.orientation.turns, 1);
        CHECK(seen.source == "wl_output test");
        // Said once: the capture starts over, with a watch of its own.
        CHECK(!watch.take(seen));
        // And the thread stopped asking once it saw it.
        const int after = watch.readings();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        CHECK_EQ(watch.readings(), after);
    }

    SECTION("TurnWatch — a turned display put back is a turn too");
    {
        auto fake = std::make_shared<FakeCompositor>();
        fake->turns = 3;
        TurnWatch watch(fake->reader(fake), Orientation{3, false}, 5);
        fake->turns = 0;
        CompositorTransform seen;
        CHECK(takeWithin(watch, seen, 500));
        CHECK(seen.orientation.upright());
    }

    SECTION("TurnWatch — a mirror alone is a change");
    {
        TurnWatch watch(
            [] {
                CompositorTransform t;
                t.known = true;
                t.orientation = Orientation{0, true};
                return t;
            },
            Orientation{}, 5);
        CompositorTransform seen;
        CHECK(takeWithin(watch, seen, 500));
        CHECK(seen.orientation.mirrored);
    }

    SECTION("TurnWatch — a compositor that stops answering is no turn");
    {
        auto fake = std::make_shared<FakeCompositor>();
        fake->answers = false;
        fake->turns = 2;
        TurnWatch watch(fake->reader(fake), Orientation{}, 5);
        CompositorTransform seen;
        CHECK(!takeWithin(watch, seen, 60));
        // It answers again, turned: seen.
        fake->answers = true;
        CHECK(takeWithin(watch, seen, 500));
        CHECK_EQ(seen.orientation.turns, 2);
    }

    SECTION("TurnWatch — the capture loop never waits on the compositor");
    {
        auto fake = std::make_shared<FakeCompositor>();
        fake->blockMs = 150;
        TurnWatch watch(fake->reader(fake), Orientation{}, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(20)); // a read in flight
        CompositorTransform seen;
        const auto start = Clock::now();
        for (int i = 0; i < 100; ++i)
            watch.take(seen);
        const auto tookMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        CHECK(tookMs < 50);
    }

    SECTION("TurnWatch — a compositor that hangs does not hold the stop");
    {
        auto fake = std::make_shared<FakeCompositor>();
        fake->blockMs = 400;
        const auto start = Clock::now();
        {
            TurnWatch watch(fake->reader(fake), Orientation{}, 1, /*stopBudgetMs=*/30);
            std::this_thread::sleep_for(std::chrono::milliseconds(20)); // a read in flight
        }
        const auto tookMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        CHECK(tookMs < 300);
        // The thread left behind ends on its own; let it, before the process
        // moves on (it holds only its own state and the stand-in).
        std::this_thread::sleep_for(std::chrono::milliseconds(450));
    }

    SECTION("TurnWatch — a read in flight is waited for, within the budget");
    {
        auto fake = std::make_shared<FakeCompositor>();
        fake->blockMs = 40;
        const auto start = Clock::now();
        {
            TurnWatch watch(fake->reader(fake), Orientation{}, 1, /*stopBudgetMs=*/1000);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto tookMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        CHECK(tookMs < 900);
    }
}
