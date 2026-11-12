/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * `moonlightweb --wait-link`, the wait the installer's last page spends before
 * it offers to open the admin page: over as soon as the server has a verdict,
 * never longer than its ceiling, and cut short for a server that says nothing.
 */
#include "test_framework.h"
#include "server/OwnerLinkWait.h"

void run_owner_link_wait_tests()
{
    using OwnerLinkWait::Answer;
    using OwnerLinkWait::kCeilingMs;
    using OwnerLinkWait::kSilentMs;
    using OwnerLinkWait::Next;
    using OwnerLinkWait::next;

    SECTION("owner's link wait: a verdict ends it at once");

    CHECK(next(Answer::Settled, true, 0) == Next::Done);
    // Whenever it comes, the ceiling included: the answer is in hand.
    CHECK(next(Answer::Settled, true, kCeilingMs + 5000) == Next::Done);

    SECTION("owner's link wait: 'not yet' is asked again, up to the ceiling");

    CHECK(next(Answer::Pending, true, 0) == Next::AskAgain);
    // About what a slow machine needs to line up and check its entry page.
    CHECK(next(Answer::Pending, true, 11000) == Next::AskAgain);
    // Past the point where a silent server is given up: this one answers.
    CHECK(next(Answer::Pending, true, kSilentMs) == Next::AskAgain);
    CHECK(next(Answer::Pending, true, kCeilingMs - 1) == Next::AskAgain);
    CHECK(next(Answer::Pending, true, kCeilingMs) == Next::GiveUp);

    SECTION("owner's link wait: a server that says nothing");

    // Still restarting: asked again, for a while.
    CHECK(next(Answer::Silent, false, 0) == Next::AskAgain);
    CHECK(next(Answer::Silent, false, kSilentMs - 1) == Next::AskAgain);
    // Never answered: it is not coming back up.
    CHECK(next(Answer::Silent, false, kSilentMs) == Next::GiveUp);
    // It answered before and went quiet (a restart in between): the ceiling
    // alone ends that wait.
    CHECK(next(Answer::Silent, true, kSilentMs) == Next::AskAgain);
    CHECK(next(Answer::Silent, true, kCeilingMs - 1) == Next::AskAgain);
    CHECK(next(Answer::Silent, true, kCeilingMs) == Next::GiveUp);

    SECTION("owner's link wait: a refusal is not asked twice");

    CHECK(next(Answer::Refused, true, 0) == Next::GiveUp);
    CHECK(next(Answer::Refused, true, 5000) == Next::GiveUp);

    SECTION("owner's link wait: its bounds");

    // The server stops saying "not yet" twenty seconds after its start: the
    // ceiling leaves room for that and for the start itself.
    CHECK(kCeilingMs > 20000);
    CHECK(kSilentMs < kCeilingMs);
}
