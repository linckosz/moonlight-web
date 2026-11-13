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

#include "core/KmsVerdictCache.h"
#include "native_test_framework.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using mw::native::kKmsVerdictTtl;
using mw::native::KmsVerdictCache;
using mw::native::kmsVerdicts;

// The Linux probe's answer to "can this card's scanout be read", kept a few
// seconds per card (KmsVerdictCache.h). No machine needed: the check is a
// counter, the clock is a value the test moves.

namespace {

using Clock = KmsVerdictCache::Clock;
using std::chrono::milliseconds;

const Clock::time_point kT0 = Clock::time_point{} + std::chrono::hours(1);

/// A clock the test sets by hand.
struct HandClock
{
    Clock::time_point now = kT0;
    KmsVerdictCache::Now reader()
    {
        return [this] { return now; };
    }
};

} // namespace

void run_kms_verdict_cache_tests()
{
    SECTION("KmsVerdictCache — asked once, then kept for the ttl");
    {
        HandClock clock;
        KmsVerdictCache cache(milliseconds(3000), clock.reader());
        int asked = 0;
        auto readable = [&asked](std::string&) {
            ++asked;
            return true;
        };
        CHECK(cache.get("/dev/dri/card1", readable).readable);
        clock.now = kT0 + milliseconds(10);
        CHECK(cache.get("/dev/dri/card1", readable).readable);
        clock.now = kT0 + milliseconds(2999);
        CHECK(cache.get("/dev/dri/card1", readable).readable);
        CHECK_EQ(asked, 1);
    }

    SECTION("KmsVerdictCache — asked again once the ttl has passed");
    {
        HandClock clock;
        KmsVerdictCache cache(milliseconds(3000), clock.reader());
        int asked = 0;
        bool answer = true;
        auto check = [&](std::string& why) {
            ++asked;
            if (!answer) why = "no framebuffer is being scanned out";
            return answer;
        };
        CHECK(cache.get("/dev/dri/card1", check).readable);
        answer = false; // the screen went off
        clock.now = kT0 + milliseconds(3000);
        const KmsVerdictCache::Verdict later = cache.get("/dev/dri/card1", check);
        CHECK(!later.readable);
        CHECK_EQ(later.why, std::string("no framebuffer is being scanned out"));
        CHECK_EQ(asked, 2);
        // And the new answer is the one kept from there.
        answer = true;
        clock.now = kT0 + milliseconds(4000);
        CHECK(!cache.get("/dev/dri/card1", check).readable);
        CHECK_EQ(asked, 2);
    }

    SECTION("KmsVerdictCache — a refusal is kept with its reason");
    {
        HandClock clock;
        KmsVerdictCache cache(milliseconds(3000), clock.reader());
        int asked = 0;
        auto refused = [&asked](std::string& why) {
            ++asked;
            why = "the helper refused: the seat is not this user's";
            return false;
        };
        const KmsVerdictCache::Verdict first = cache.get("/dev/dri/card0", refused);
        clock.now = kT0 + milliseconds(500);
        const KmsVerdictCache::Verdict second = cache.get("/dev/dri/card0", refused);
        CHECK(!first.readable);
        CHECK(!second.readable);
        CHECK_EQ(second.why, first.why);
        CHECK_EQ(second.why, std::string("the helper refused: the seat is not this user's"));
        CHECK_EQ(asked, 1);
    }

    SECTION("KmsVerdictCache — a readable card carries no reason");
    {
        // canReadFramebuffers may leave a word in `why` on its way to a yes;
        // the probe reads `why` only on a no, and the kept verdict says nothing.
        HandClock clock;
        KmsVerdictCache cache(milliseconds(3000), clock.reader());
        const KmsVerdictCache::Verdict v = cache.get("/dev/dri/card1", [](std::string& why) {
            why = "first plane had no buffer";
            return true;
        });
        CHECK(v.readable);
        CHECK(v.why.empty());
    }

    SECTION("KmsVerdictCache — each card has its own verdict and its own age");
    {
        HandClock clock;
        KmsVerdictCache cache(milliseconds(3000), clock.reader());
        int askedCard0 = 0;
        int askedCard1 = 0;
        auto card0 = [&askedCard0](std::string& why) {
            ++askedCard0;
            why = "no framebuffer is being scanned out on /dev/dri/card0";
            return false;
        };
        auto card1 = [&askedCard1](std::string&) {
            ++askedCard1;
            return true;
        };
        CHECK(!cache.get("/dev/dri/card0", card0).readable);
        clock.now = kT0 + milliseconds(2000);
        CHECK(cache.get("/dev/dri/card1", card1).readable);
        // At 3.5 s card0's verdict is stale, card1's is not.
        clock.now = kT0 + milliseconds(3500);
        CHECK(!cache.get("/dev/dri/card0", card0).readable);
        CHECK(cache.get("/dev/dri/card1", card1).readable);
        CHECK_EQ(askedCard0, 2);
        CHECK_EQ(askedCard1, 1);
    }

    SECTION("KmsVerdictCache — a clock that reads before the kept time asks again");
    {
        HandClock clock;
        KmsVerdictCache cache(milliseconds(3000), clock.reader());
        int asked = 0;
        auto readable = [&asked](std::string&) {
            ++asked;
            return true;
        };
        CHECK(cache.get("/dev/dri/card1", readable).readable);
        clock.now = kT0 - milliseconds(1);
        CHECK(cache.get("/dev/dri/card1", readable).readable);
        CHECK_EQ(asked, 2);
    }

    SECTION("KmsVerdictCache — callers at the same moment share one check");
    {
        // A page that fires its requests together: the first asks the card,
        // the others wait for that answer instead of starting a helper each.
        // The clock moves at every read, as a real one does between threads,
        // and the first caller is held a while just after reading it: had the
        // time been read before the lock, a second caller would take the lock
        // meanwhile and keep its answer at a later time than the first one
        // read, which would then find that answer "from the future" and ask
        // again — two helpers for one moment.
        std::atomic<int> reads{0};
        KmsVerdictCache cache(milliseconds(3000), [&reads] {
            const int n = reads.fetch_add(1);
            if (n == 0) std::this_thread::sleep_for(milliseconds(20));
            return kT0 + milliseconds(n);
        });
        std::atomic<int> asked{0};
        std::atomic<int> readableSeen{0};
        auto slow = [&asked](std::string&) {
            ++asked;
            std::this_thread::sleep_for(milliseconds(30));
            return true;
        };
        std::vector<std::thread> callers;
        for (int i = 0; i < 8; ++i) {
            callers.emplace_back([&] {
                if (cache.get("/dev/dri/card1", slow).readable) ++readableSeen;
            });
        }
        for (std::thread& t : callers)
            t.join();
        CHECK_EQ(asked.load(), 1);
        CHECK_EQ(readableSeen.load(), 8);
        CHECK_EQ(reads.load(), 8);
    }

    SECTION("KmsVerdictCache — clear() makes the next get() ask again");
    {
        // What a session's creation does before its probe: the capture it
        // chooses rests on the answer of now, not on one from a second ago.
        HandClock clock;
        KmsVerdictCache cache(milliseconds(3000), clock.reader());
        int asked = 0;
        bool answer = false;
        auto check = [&](std::string& why) {
            ++asked;
            if (!answer) why = "the helper refused: the seat is not this user's";
            return answer;
        };
        CHECK(!cache.get("/dev/dri/card1", check).readable);
        answer = true; // the seat came back
        clock.now = kT0 + milliseconds(100);
        CHECK(!cache.get("/dev/dri/card1", check).readable);
        CHECK_EQ(asked, 1);
        cache.clear();
        CHECK(cache.get("/dev/dri/card1", check).readable);
        CHECK_EQ(asked, 2);
    }

    SECTION("KmsVerdictCache — the process's verdicts are one cache, and clear() reaches it");
    {
        CHECK(&kmsVerdicts() == &kmsVerdicts());
        int asked = 0;
        auto readable = [&asked](std::string&) {
            ++asked;
            return true;
        };
        // A card name no machine has, so that no probe of this run shares it.
        const std::string card = "/dev/dri/test-kms-verdict-cache";
        kmsVerdicts().clear();
        CHECK(kmsVerdicts().get(card, readable).readable);
        CHECK(kmsVerdicts().get(card, readable).readable);
        CHECK_EQ(asked, 1);
        kmsVerdicts().clear();
        CHECK(kmsVerdicts().get(card, readable).readable);
        CHECK_EQ(asked, 2);
        kmsVerdicts().clear();
    }

    SECTION("KmsVerdictCache — the shipped ttl is a few seconds");
    {
        CHECK(kKmsVerdictTtl >= milliseconds(2000));
        CHECK(kKmsVerdictTtl <= milliseconds(5000));
        // The cache with the shipped ttl, the probe's, keeps for that long.
        HandClock clock;
        KmsVerdictCache cache(kKmsVerdictTtl, clock.reader());
        int asked = 0;
        auto readable = [&asked](std::string&) {
            ++asked;
            return true;
        };
        (void)cache.get("/dev/dri/card1", readable);
        clock.now = kT0 + kKmsVerdictTtl - milliseconds(1);
        (void)cache.get("/dev/dri/card1", readable);
        CHECK_EQ(asked, 1);
        clock.now = kT0 + kKmsVerdictTtl;
        (void)cache.get("/dev/dri/card1", readable);
        CHECK_EQ(asked, 2);
    }
}
