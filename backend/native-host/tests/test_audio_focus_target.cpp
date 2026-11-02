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

#include "audio/FocusTarget.h"
#include "mw/native/AudioSource.h"
#include "native_test_framework.h"

#include <cstdint>

using mw::native::AudioSource;
using mw::native::audioSourceFromString;
using mw::native::audio::FocusSample;
using mw::native::audio::FocusTarget;
using mw::native::audio::nextFocusTarget;

namespace {

constexpr uint32_t kGame = 100;
constexpr uint32_t kDiscord = 200;
constexpr uint32_t kBrowser = 300;
constexpr uint32_t kShell = 400;

FocusSample windowed(uint32_t pid)
{
    return FocusSample{pid, false, false};
}
FocusSample fullscreen(uint32_t pid)
{
    return FocusSample{pid, true, false};
}
FocusSample shell()
{
    return FocusSample{kShell, true, true};
}

} // namespace

void run_audio_focus_target_tests()
{
    SECTION("AudioSource: only \"focused\" turns the mode on");
    CHECK(audioSourceFromString("focused") == AudioSource::FocusedApp);
    CHECK(audioSourceFromString("all") == AudioSource::AllApps);
    CHECK(audioSourceFromString("") == AudioSource::AllApps);
    CHECK(audioSourceFromString("Focused") == AudioSource::AllApps);

    SECTION("FocusTarget: nothing yet → the windowed app in front");
    FocusTarget t = nextFocusTarget(FocusTarget{}, false, windowed(kBrowser));
    CHECK_EQ(t.pid, kBrowser);
    CHECK(!t.fullscreen);

    SECTION("FocusTarget: a windowed target follows focus");
    t = nextFocusTarget(t, true, windowed(kDiscord));
    CHECK_EQ(t.pid, kDiscord);

    SECTION("FocusTarget: a full-screen app always takes over");
    t = nextFocusTarget(t, true, fullscreen(kGame));
    CHECK_EQ(t.pid, kGame);
    CHECK(t.fullscreen);

    SECTION("FocusTarget: alt-tab to a windowed app keeps the game");
    t = nextFocusTarget(t, true, windowed(kDiscord));
    CHECK_EQ(t.pid, kGame);
    t = nextFocusTarget(t, true, windowed(kBrowser));
    CHECK_EQ(t.pid, kGame);

    SECTION("FocusTarget: the shell or no window in front changes nothing");
    t = nextFocusTarget(t, true, shell());
    CHECK_EQ(t.pid, kGame);
    t = nextFocusTarget(t, true, FocusSample{});
    CHECK_EQ(t.pid, kGame);

    SECTION("FocusTarget: the game in a window again stays sticky");
    t = nextFocusTarget(t, true, windowed(kGame));
    CHECK_EQ(t.pid, kGame);
    CHECK(t.fullscreen);

    SECTION("FocusTarget: another full-screen app replaces the game");
    t = nextFocusTarget(t, true, fullscreen(kBrowser));
    CHECK_EQ(t.pid, kBrowser);
    CHECK(t.fullscreen);

    SECTION("FocusTarget: a target that exited is dropped — silence, not the mix");
    t = nextFocusTarget(t, false, shell());
    CHECK_EQ(t.pid, 0u);
    t = nextFocusTarget(t, false, FocusSample{});
    CHECK_EQ(t.pid, 0u);

    SECTION("FocusTarget: after an exit, the next app in front is taken");
    t = nextFocusTarget(FocusTarget{kGame, true}, false, windowed(kDiscord));
    CHECK_EQ(t.pid, kDiscord);
    CHECK(!t.fullscreen);

    SECTION("FocusTarget: a windowed app gone full screen becomes sticky");
    t = nextFocusTarget(FocusTarget{kGame, false}, true, fullscreen(kGame));
    CHECK_EQ(t.pid, kGame);
    CHECK(t.fullscreen);
    t = nextFocusTarget(t, true, windowed(kDiscord));
    CHECK_EQ(t.pid, kGame);
}
