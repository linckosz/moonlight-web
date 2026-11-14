/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "input/NaturalScroll.h"

using namespace mw::native::input;

// A GNOME host with natural scrolling on reversed our wheel in all four
// directions (UM790Pro, 12/11/2026): the host reads the switch through
// gsettings and sends the wheel the other way. These are the two pure
// decisions around that read — which desktop asks for it, and which answers
// count. Pure strings, tested on every platform.

void run_natural_scroll_tests()
{
    SECTION("Natural scroll — GNOME is found in XDG_CURRENT_DESKTOP");

    CHECK(desktopIsGnome("GNOME"));
    CHECK(desktopIsGnome("ubuntu:GNOME"));
    CHECK(desktopIsGnome("GNOME-Classic:GNOME"));
    CHECK(desktopIsGnome("gnome"));
    CHECK(desktopIsGnome("Pop:GNOME"));

    // Other desktops, and names that only look like it.
    CHECK(!desktopIsGnome("KDE"));
    CHECK(!desktopIsGnome("X-Cinnamon"));
    CHECK(!desktopIsGnome("GNOME-Classic"));
    CHECK(!desktopIsGnome("GNOMEish"));
    CHECK(!desktopIsGnome("ubuntu"));
    CHECK(!desktopIsGnome(""));
    CHECK(!desktopIsGnome(":"));
    CHECK(!desktopIsGnome(nullptr));

    SECTION("Natural scroll — gsettings is never run as root or set-uid");

    CHECK(mayAskGsettings(1000, 1000));
    CHECK(!mayAskGsettings(0, 0));
    CHECK(!mayAskGsettings(1000, 0));
    CHECK(!mayAskGsettings(0, 1000));
    CHECK(!mayAskGsettings(1000, 1001));

    SECTION("Natural scroll — gsettings gets the user's home back from the launcher");

    {
        // The stream worker's environment, as moonlightweb-launch builds it.
        const std::vector<std::string> env = gsettingsEnvironment(
            {"HOME=/nonexistent", "XDG_CONFIG_HOME=/nonexistent", "XDG_RUNTIME_DIR=/run/user/1000",
             "MW_USER_HOME=/home/bruno", "XDG_CURRENT_DESKTOP=ubuntu:GNOME"});
        auto has = [&](const char* e) {
            for (const std::string& s : env)
                if (s == e) return true;
            return false;
        };
        CHECK(has("HOME=/home/bruno"));
        CHECK(!has("HOME=/nonexistent"));
        CHECK(!has("XDG_CONFIG_HOME=/nonexistent"));
        CHECK(has("XDG_RUNTIME_DIR=/run/user/1000"));
        CHECK(has("XDG_CURRENT_DESKTOP=ubuntu:GNOME"));
        CHECK(env.size() == 4);
    }
    {
        // No launcher: the environment goes as it is, XDG_CONFIG_HOME included.
        const std::vector<std::string> env =
            gsettingsEnvironment({"HOME=/home/bruno", "XDG_CONFIG_HOME=/home/bruno/.cfg"});
        CHECK(env.size() == 2);
        CHECK(env[0] == "HOME=/home/bruno");
        CHECK(env[1] == "XDG_CONFIG_HOME=/home/bruno/.cfg");
    }
    // A home that is not an absolute path free of ".." is not taken.
    CHECK(gsettingsEnvironment({"HOME=/nonexistent", "MW_USER_HOME=home/x"})[0] ==
          "HOME=/nonexistent");
    CHECK(gsettingsEnvironment({"HOME=/nonexistent", "MW_USER_HOME=/home/../root"})[0] ==
          "HOME=/nonexistent");
    CHECK(gsettingsEnvironment({"HOME=/nonexistent", "MW_USER_HOME="})[0] == "HOME=/nonexistent");

    SECTION("Natural scroll — only \"true\" or \"false\" is an answer");

    CHECK(parseGsettingsBool("true\n") == std::optional<bool>(true));
    CHECK(parseGsettingsBool("false\n") == std::optional<bool>(false));
    CHECK(parseGsettingsBool("true") == std::optional<bool>(true));
    CHECK(parseGsettingsBool(" false \r\n") == std::optional<bool>(false));

    // An error, a missing schema or nothing at all is no answer: the wheel
    // keeps the direction it had.
    CHECK(!parseGsettingsBool(""));
    CHECK(!parseGsettingsBool("\n"));
    CHECK(!parseGsettingsBool("No such schema \"org.gnome.desktop.peripherals.mouse\"\n"));
    CHECK(!parseGsettingsBool("No such key \"natural-scroll\"\n"));
    CHECK(!parseGsettingsBool("True\n"));
    CHECK(!parseGsettingsBool("truefalse"));
    CHECK(!parseGsettingsBool("1\n"));
}
