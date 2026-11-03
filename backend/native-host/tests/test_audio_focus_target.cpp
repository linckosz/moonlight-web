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
#include <string>
#include <vector>

using mw::native::AudioSource;
using mw::native::isWindowsShellExe;
using mw::native::parseAudioSource;
using mw::native::audio::FocusSample;
using mw::native::audio::FocusTarget;
using mw::native::audio::nextFocusTarget;
using mw::native::audio::pickAppRoot;
using mw::native::audio::ProcessEntry;

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

/// The same sample on the two polls an app needs to take over.
FocusTarget twoPolls(FocusTarget t, const FocusSample& front)
{
    return nextFocusTarget(nextFocusTarget(t, true, front), true, front);
}

} // namespace

void run_audio_focus_target_tests()
{
    SECTION("AudioSource: the settings value");
    CHECK(parseAudioSource("focused").source == AudioSource::FocusedApp);
    CHECK(parseAudioSource("all").source == AudioSource::AllApps);
    CHECK(parseAudioSource("").source == AudioSource::AllApps);
    CHECK(parseAudioSource("Focused").source == AudioSource::AllApps);
    CHECK(parseAudioSource("app:").source == AudioSource::AllApps);
    CHECK(parseAudioSource("app:Discord.EXE").source == AudioSource::OneApp);
    CHECK(parseAudioSource("app:Discord.EXE").app == "discord.exe");

    SECTION("Shell list: the overlays and prompts never take the sound");
    CHECK(isWindowsShellExe("explorer.exe"));
    CHECK(isWindowsShellExe("consent.exe"));
    CHECK(isWindowsShellExe("gamebar.exe"));
    CHECK(isWindowsShellExe("nvidia overlay.exe"));
    CHECK(!isWindowsShellExe("discord.exe"));
    CHECK(!isWindowsShellExe(""));

    SECTION("FocusTarget: the app in front takes over on the second poll");
    FocusTarget t = nextFocusTarget(FocusTarget{}, true, here(kGame));
    CHECK_EQ(t.pid, 0u);
    CHECK_EQ(t.candidate, kGame);
    t = nextFocusTarget(t, true, here(kGame));
    CHECK_EQ(t.pid, kGame);
    CHECK_EQ(t.candidate, 0u);

    SECTION("FocusTarget: a window flashing through the focus changes nothing");
    t = nextFocusTarget(t, true, here(kBrowser));
    CHECK_EQ(t.pid, kGame);
    t = nextFocusTarget(t, true, here(kGame));
    CHECK_EQ(t.pid, kGame);
    CHECK_EQ(t.candidate, 0u);
    t = nextFocusTarget(t, true, here(kBrowser));
    t = nextFocusTarget(t, true, here(kDiscord));
    CHECK_EQ(t.pid, kGame);
    CHECK_EQ(t.candidate, kDiscord);
    t = nextFocusTarget(t, true, here(kGame));

    SECTION("FocusTarget: alt-tab on the streamed screen switches to that app");
    t = twoPolls(t, here(kBrowser));
    CHECK_EQ(t.pid, kBrowser);
    t = twoPolls(t, here(kGame));
    CHECK_EQ(t.pid, kGame);

    SECTION("FocusTarget: focus on another screen keeps the current app");
    t = twoPolls(t, otherScreen(kDiscord));
    CHECK_EQ(t.pid, kGame);

    SECTION("FocusTarget: the shell or no window in front changes nothing");
    t = twoPolls(t, shell());
    CHECK_EQ(t.pid, kGame);
    t = twoPolls(t, FocusSample{});
    CHECK_EQ(t.pid, kGame);

    SECTION("FocusTarget: a shell, empty or other-screen sample resets the candidate");
    t = nextFocusTarget(t, true, here(kBrowser));
    t = nextFocusTarget(t, true, shell());
    CHECK_EQ(t.candidate, 0u);
    t = nextFocusTarget(t, true, here(kBrowser));
    CHECK_EQ(t.pid, kGame);
    t = nextFocusTarget(t, true, FocusSample{});
    CHECK_EQ(t.candidate, 0u);
    t = nextFocusTarget(t, true, here(kBrowser));
    t = nextFocusTarget(t, true, otherScreen(kBrowser));
    CHECK_EQ(t.candidate, 0u);
    CHECK_EQ(t.pid, kGame);

    SECTION("FocusTarget: the game closes or crashes: silence at once, never the mix");
    t = nextFocusTarget(t, false, shell());
    CHECK_EQ(t.pid, 0u);
    t = twoPolls(t, otherScreen(kDiscord));
    CHECK_EQ(t.pid, 0u);

    SECTION("FocusTarget: the next game in front is taken");
    t = twoPolls(t, here(kNextGame));
    CHECK_EQ(t.pid, kNextGame);

    SECTION("FocusTarget: a target that exited while focus stays elsewhere is dropped");
    t = nextFocusTarget(FocusTarget{kGame, 0}, false, otherScreen(kDiscord));
    CHECK_EQ(t.pid, 0u);

    SECTION("FocusTarget: a target that cannot be told closed stays through another screen");
    t = nextFocusTarget(FocusTarget{kGame, 0}, true, otherScreen(kDiscord));
    CHECK_EQ(t.pid, kGame);

    SECTION("pickAppRoot: the first process not started by another of the same app");
    const std::vector<ProcessEntry> processes = {
        {10, 1, "explorer.exe"}, {20, 10, "update.exe"},  {30, 20, "discord.exe"},
        {31, 30, "discord.exe"}, {32, 30, "discord.exe"}, {40, 10, "chrome.exe"},
        {41, 40, "chrome.exe"},  {50, 999, "orphan.exe"}, {60, 60, "selfparent.exe"},
    };
    CHECK_EQ(pickAppRoot(processes, "discord.exe"), 30u);
    CHECK_EQ(pickAppRoot(processes, "chrome.exe"), 40u);
    CHECK_EQ(pickAppRoot(processes, "orphan.exe"), 50u);
    CHECK_EQ(pickAppRoot(processes, "selfparent.exe"), 60u);
    CHECK_EQ(pickAppRoot(processes, "notrunning.exe"), 0u);
    CHECK_EQ(pickAppRoot({}, "discord.exe"), 0u);
}
