/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Issue #24: on a host whose app outlives the stream, the app the host runs
 * decides between /launch and /resume. Sunshine refuses /launch while any app
 * runs and /resume joins whatever runs, so a wrong answer here reconnects the
 * viewer to an app they did not click.
 */
#include "test_framework.h"
#include "streaming/StartChoice.h"

using startchoice::decide;
using startchoice::Verb;

void run_start_choice_tests()
{
    SECTION("StartChoice");

    // Nothing runs: launch what was asked.
    CHECK(decide(0, 42, false) == Verb::Launch);
    // The app asked for already runs (Stop left it running): join it.
    CHECK(decide(42, 42, false) == Verb::Resume);
    // Another app runs: never join it in place of the one asked for.
    CHECK(decide(7, 42, false) == Verb::AppRunning);

    // After our /launch was refused, the same rules hold for a running app...
    CHECK(decide(42, 42, true) == Verb::Resume);
    CHECK(decide(7, 42, true) == Verb::AppRunning);
    // ...but "nothing runs" cannot explain the refusal: the older rules decide.
    CHECK(decide(0, 42, true) == Verb::ByHint);
}
