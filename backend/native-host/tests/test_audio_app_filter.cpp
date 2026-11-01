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

#include "audio/AppTreeSelect.h"
#include "mw/native/AudioAppFilter.h"
#include "native_test_framework.h"

#include <map>
#include <set>
#include <string>
#include <vector>

using mw::native::AudioAppFilter;
using mw::native::audio::chooseCaptureTrees;

namespace {

/// A process tree as parent links; ancestors() walks it nearest first.
struct FakeTree
{
    std::map<uint32_t, uint32_t> parent;

    std::vector<uint32_t> ancestors(uint32_t pid) const
    {
        std::vector<uint32_t> out;
        for (auto it = parent.find(pid); it != parent.end(); it = parent.find(it->second))
            out.push_back(it->second);
        return out;
    }
};

} // namespace

void run_audio_app_filter_tests()
{
    SECTION("AudioAppFilter: names are reduced to a lower-case file name");
    CHECK_EQ(AudioAppFilter::normalise("Chrome.EXE"), std::string("chrome.exe"));
    CHECK_EQ(AudioAppFilter::normalise("  discord.exe \t"), std::string("discord.exe"));
    CHECK_EQ(AudioAppFilter::normalise("C:\\Program Files\\Mozilla Firefox\\firefox.exe"),
             std::string("firefox.exe"));
    CHECK_EQ(AudioAppFilter::normalise("/usr/bin/Foo"), std::string("foo"));
    CHECK_EQ(AudioAppFilter::normalise(""), std::string());

    SECTION("AudioAppFilter: set() de-duplicates and bumps the generation only on change");
    AudioAppFilter::set({"Chrome.exe", "chrome.exe", "", "Discord.exe"});
    const uint64_t g1 = AudioAppFilter::generation();
    CHECK(g1 != 0);
    CHECK_EQ(AudioAppFilter::get(), (std::vector<std::string>{"chrome.exe", "discord.exe"}));
    AudioAppFilter::set({"discord.exe", "CHROME.EXE"});
    CHECK_EQ(AudioAppFilter::generation(), g1);
    AudioAppFilter::set({"discord.exe"});
    CHECK(AudioAppFilter::generation() != g1);
    AudioAppFilter::set({});
    CHECK(AudioAppFilter::get().empty());

    // explorer(10) ─┬─ discord(20, excluded) ── discord helper(21, excluded)
    //               ├─ steam(30) ── game(31)
    //               └─ spotify(40)
    // game(31) and steam(30) both own a session; so do explorer, spotify.
    FakeTree tree;
    tree.parent = {{20, 10}, {21, 20}, {30, 10}, {31, 30}, {40, 10}};
    const auto anc = [&tree](uint32_t pid) { return tree.ancestors(pid); };

    SECTION("chooseCaptureTrees: an owner with an excluded app under it is never opened");
    {
        const auto c = chooseCaptureTrees({10, 30, 31, 40}, {20, 21}, anc);
        CHECK(c.unsafe.count(10) == 1);
        CHECK(c.wanted.count(10) == 0);
        // steam's tree carries the game: one client, not two.
        CHECK(c.wanted.count(30) == 1);
        CHECK(c.wanted.count(31) == 0);
        CHECK(c.wanted.count(40) == 1);
        CHECK_EQ(c.wanted.size(), size_t(2));
    }

    SECTION("chooseCaptureTrees: with nothing excluded the topmost owner covers all");
    {
        const auto c = chooseCaptureTrees({10, 30, 31, 40}, {}, anc);
        CHECK_EQ(c.wanted, (std::set<uint32_t>{10}));
        CHECK(c.unsafe.empty());
    }

    SECTION("chooseCaptureTrees: an excluded app's own child is still heard");
    {
        // A game launched from Discord's library: not excluded itself.
        FakeTree t2;
        t2.parent = {{20, 10}, {22, 20}};
        const auto c =
            chooseCaptureTrees({22}, {20}, [&t2](uint32_t pid) { return t2.ancestors(pid); });
        CHECK_EQ(c.wanted, (std::set<uint32_t>{22}));
    }

    SECTION("chooseCaptureTrees: an owner whose ancestor is unsafe stands on its own");
    {
        const auto c = chooseCaptureTrees({10, 31}, {20}, anc);
        CHECK(c.unsafe.count(10) == 1);
        CHECK_EQ(c.wanted, (std::set<uint32_t>{31}));
    }
}
