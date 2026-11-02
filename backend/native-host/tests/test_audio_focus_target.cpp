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
constexpr uint32_t kNextGame = 500;

FocusSample here(uint32_t pid)
{
    return FocusSample{pid, true, false};
}
FocusSample otherScreen(uint32_t pid)
{
    return FocusSample{pid, false, false};
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

    SECTION("FocusTarget: the app in front on the streamed screen is the target");
    FocusTarget t = nextFocusTarget(FocusTarget{}, false, here(kGame));
    CHECK_EQ(t.pid, kGame);

    SECTION("FocusTarget: alt-tab on the streamed screen switches to that app");
    t = nextFocusTarget(t, true, here(kBrowser));
    CHECK_EQ(t.pid, kBrowser);
    t = nextFocusTarget(t, true, here(kGame));
    CHECK_EQ(t.pid, kGame);

    SECTION("FocusTarget: focus on another screen keeps the current app");
    t = nextFocusTarget(t, true, otherScreen(kDiscord));
    CHECK_EQ(t.pid, kGame);

    SECTION("FocusTarget: the shell or no window in front changes nothing");
    t = nextFocusTarget(t, true, shell());
    CHECK_EQ(t.pid, kGame);
    t = nextFocusTarget(t, true, FocusSample{});
    CHECK_EQ(t.pid, kGame);

    SECTION("FocusTarget: the game closes or crashes — silence, never the mix");
    t = nextFocusTarget(t, false, shell());
    CHECK_EQ(t.pid, 0u);
    t = nextFocusTarget(t, false, otherScreen(kDiscord));
    CHECK_EQ(t.pid, 0u);

    SECTION("FocusTarget: the next game in front is taken");
    t = nextFocusTarget(t, false, here(kNextGame));
    CHECK_EQ(t.pid, kNextGame);

    SECTION("FocusTarget: a target that exited while focus stays elsewhere is dropped");
    t = nextFocusTarget(FocusTarget{kGame}, false, otherScreen(kDiscord));
    CHECK_EQ(t.pid, 0u);
}
