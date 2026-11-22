/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "platform/macos/DisplayAway.h"

#include <string>

using namespace mw::native::platform::away;

// Why a Mac's display is away, in words (DisplayAway.h). The bench of
// 22/11/2026 is the case: a viewer read "ScreenCaptureKit did not start
// within 5 s" while the Mac's locked screen saver kept taking the panel back.
// Its owner is told what is wrong and what to do, behind a stable tag; what a
// guest of a shared stream is told instead is the server's business, tested
// with the backend (test_display_away_answer.cpp). Plain strings, so tested
// on every platform.

namespace {

const std::string kSck = "ScreenCaptureKit did not start within 5 s";

bool contains(const std::string& s, const char* part)
{
    return s.find(part) != std::string::npos;
}

bool startsWith(const std::string& s, const std::string& head)
{
    return s.compare(0, head.size(), head) == 0;
}

} // namespace

void run_display_away_tests()
{
    SECTION("DisplayAway — nothing known: ScreenCaptureKit's own words, untagged");
    {
        const AwayState none;
        CHECK(!known(none));
        CHECK(logReason(none).empty());
        CHECK(ownerReason(none).empty());
        CHECK_EQ(startError(none, kSck), kSck);
        CHECK(!startsWith(startError(none, kSck), kCode));
    }

    SECTION("DisplayAway — the screen saver of 22/11, locked: the cause first, then the lock");
    {
        AwayState s;
        s.screenSaver = true;
        s.locked = true;
        CHECK(known(s));
        CHECK_EQ(logReason(s), std::string("screen saver on, Mac locked"));
        CHECK_EQ(ownerReason(s),
                 std::string("the Mac's screen saver is on, and the Mac is locked — wake and "
                             "unlock it, then start the stream again"));
        const std::string error = startError(s, kSck);
        CHECK(startsWith(error, std::string(kCode) + ": "));
        CHECK(contains(error, "screen saver is on"));
        CHECK(contains(error, "locked"));
        // ScreenCaptureKit's words stay, in brackets, for whoever debugs it.
        CHECK(contains(error, "(ScreenCaptureKit did not start within 5 s)"));
    }

    SECTION("DisplayAway — each state on its own, and the advice that goes with it");
    {
        AwayState off;
        off.asleep = true;
        CHECK_EQ(logReason(off), std::string("screen off"));
        CHECK_EQ(ownerReason(off),
                 std::string("the Mac's screen is off — wake it, then start the stream again"));

        AwayState saver;
        saver.screenSaver = true;
        CHECK_EQ(ownerReason(saver), std::string("the Mac's screen saver is on — wake it, then "
                                                 "start the stream again"));

        // Locked with the panel on streamed on at the bench: said, since it is
        // all that is known, and the advice is only to unlock.
        AwayState locked;
        locked.locked = true;
        CHECK_EQ(ownerReason(locked),
                 std::string("the Mac is locked — unlock it, then start the stream again"));

        AwayState both;
        both.asleep = true;
        both.screenSaver = true;
        CHECK_EQ(logReason(both), std::string("screen off, screen saver on"));
        CHECK(startsWith(ownerReason(both),
                         "the Mac's screen is off and its screen saver is on — wake it"));

        AwayState all;
        all.asleep = true;
        all.screenSaver = true;
        all.locked = true;
        CHECK_EQ(logReason(all), std::string("screen off, screen saver on, Mac locked"));
        CHECK(contains(ownerReason(all), "wake and unlock it"));
    }

    SECTION("DisplayAway — no capture error to quote: the reason alone, still tagged");
    {
        AwayState s;
        s.asleep = true;
        CHECK_EQ(startError(s, std::string()),
                 std::string(kCode) +
                     ": the Mac's screen is off — wake it, then start the stream again");
    }

    SECTION("DisplayAway — every known state is tagged, so the server can sort it out");
    {
        AwayState cases[7];
        cases[0].asleep = true;
        cases[1].screenSaver = true;
        cases[2].locked = true;
        cases[3].asleep = cases[3].screenSaver = true;
        cases[4].asleep = cases[4].locked = true;
        cases[5].screenSaver = cases[5].locked = true;
        cases[6].asleep = cases[6].screenSaver = cases[6].locked = true;
        for (const AwayState& s : cases) {
            const std::string error = startError(s, kSck);
            // "tag: text", the tag first: what DisplayAwayAnswer.h looks for.
            CHECK(startsWith(error, std::string(kCode) + ": "));
            CHECK(contains(error, "then start the stream again"));
        }
    }
}
