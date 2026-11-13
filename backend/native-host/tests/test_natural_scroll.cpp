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
