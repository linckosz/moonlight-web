/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "streaming/RemoteDesktopReturnPolicy.h"

using namespace RemoteDesktopReturnPolicy;

namespace {

/// What the launcher sees when evo closes mstsc: the caller's session 1 is
/// Disconnected, the console is an empty session 8 at the sign-in screen.
ConsoleReturnFacts evo()
{
    ConsoleReturnFacts f;
    f.callerIsThisExecutable = true;
    f.callerSession = 1;
    f.callerState = Disconnected;
    f.consoleSession = 8;
    f.consoleUserPresent = false;
    return f;
}

Observation seen(int state, bool remote = false)
{
    Observation o;
    o.state = state;
    o.remote = remote;
    return o;
}

} // namespace

// The launcher gains a SYSTEM power here — connecting a session to the
// console without its password — so every refusal is pinned: it moves only
// the caller's own session, only when Remote Desktop left it Disconnected, and
// never over a console somebody is signed in on.
void run_remote_desktop_return_tests()
{
    SECTION("RemoteDesktopReturn — the launcher's checks");

    CHECK(consoleReturnRefusal(evo()).isEmpty());

    ConsoleReturnFacts f = evo();
    f.callerIsThisExecutable = false;
    CHECK(!consoleReturnRefusal(f).isEmpty());

    f = evo();
    f.callerSession = kNoSession;
    CHECK(!consoleReturnRefusal(f).isEmpty());

    f = evo();
    f.callerSession = 0;
    CHECK(!consoleReturnRefusal(f).isEmpty());

    f = evo();
    f.consoleSession = kNoSession;
    CHECK(!consoleReturnRefusal(f).isEmpty());

    // Already there: nothing to move.
    f = evo();
    f.consoleSession = 1;
    CHECK(!consoleReturnRefusal(f).isEmpty());

    // Remote Desktop still connected, or any state but Disconnected.
    for (int state : {int(Active), int(Connected), int(ConnectQuery), int(Shadow), int(Idle),
                      int(Listen), int(Reset), int(Down), int(Init), -1}) {
        f = evo();
        f.callerState = state;
        CHECK(!consoleReturnRefusal(f).isEmpty());
    }

    // A second account signed in on the PC keeps its screen.
    f = evo();
    f.consoleUserPresent = true;
    CHECK(!consoleReturnRefusal(f).isEmpty());

    SECTION("RemoteDesktopReturn — the way back (hide!)");

    // A return that could not lock: the caller's session is Active on the
    // console, and only that one may be sent away again.
    {
        ConsoleReturnFacts h = evo();
        h.callerState = Active;
        h.consoleSession = 1;
        h.consoleUserPresent = true; // the caller itself
        CHECK(hideRefusal(h).isEmpty());

        ConsoleReturnFacts x = h;
        x.callerIsThisExecutable = false;
        CHECK(!hideRefusal(x).isEmpty());
        x = h;
        x.callerSession = 0;
        x.consoleSession = 0;
        CHECK(!hideRefusal(x).isEmpty());
        x = h;
        x.callerSession = kNoSession;
        CHECK(!hideRefusal(x).isEmpty());
        // Not on the console: another session is there, and it is not ours.
        x = h;
        x.consoleSession = 8;
        CHECK(!hideRefusal(x).isEmpty());
        // Already away, or on its way: nothing to hide.
        for (int state : {int(Disconnected), int(Connected), int(ConnectQuery), -1}) {
            x = h;
            x.callerState = state;
            CHECK(!hideRefusal(x).isEmpty());
        }
    }

    SECTION("RemoteDesktopReturn — the server's tracker");

    // Moved but not locked, then sent away again: not asked again while it
    // stays away — a return that cannot lock would show the desktop every
    // kRetryTicks. Used again (Active), it starts over.
    {
        Tracker t;
        t.observe(seen(Active, true), true);
        t.observe(seen(Disconnected), true);
        CHECK(t.observe(seen(Disconnected), true).askReturn);
        t.giveUp();
        int asked = 0;
        for (int i = 0; i < 4 * Tracker::kRetryTicks; ++i) {
            const Decision d = t.observe(seen(Disconnected), true);
            CHECK(d.leftByRemoteDesktop);
            asked += d.askReturn ? 1 : 0;
        }
        CHECK_EQ(asked, 0);
        t.observe(seen(Active, true), true);
        t.observe(seen(Disconnected), true);
        CHECK(t.observe(seen(Disconnected), true).askReturn);
    }

    // evo's case: active through Remote Desktop, then mstsc closed. The card
    // changes on the first tick; the return is asked on the second.
    {
        Tracker t;
        Decision d = t.observe(seen(Active, true), true);
        CHECK(!d.leftByRemoteDesktop && !d.askReturn);
        d = t.observe(seen(Disconnected), true);
        CHECK(d.leftByRemoteDesktop);
        CHECK(!d.askReturn);
        d = t.observe(seen(Disconnected), true);
        CHECK(d.leftByRemoteDesktop && d.askReturn);
        // Asked once; not again on the next tick...
        d = t.observe(seen(Disconnected), true);
        CHECK(d.leftByRemoteDesktop && !d.askReturn);
        // ...and back on the console it is a session like any other.
        d = t.observe(seen(Active, false), true);
        CHECK(!d.leftByRemoteDesktop && !d.askReturn);
    }

    // Switch user on the PC: Disconnected too, but last seen on the console.
    // Pulling it back would throw the other person off the sign-in screen.
    {
        Tracker t;
        t.observe(seen(Active, false), true);
        for (int i = 0; i < 40; ++i) {
            const Decision d = t.observe(seen(Disconnected), true);
            CHECK(!d.leftByRemoteDesktop && !d.askReturn);
        }
    }

    // The server started in a session already disconnected: nothing known of
    // how it got there, so nothing done.
    {
        Tracker t;
        for (int i = 0; i < 5; ++i) {
            const Decision d = t.observe(seen(Disconnected), true);
            CHECK(!d.leftByRemoteDesktop && !d.askReturn);
        }
    }

    // The setting off (or no launcher, or the native host disabled): the card
    // still says what happened, and nothing is asked.
    {
        Tracker t;
        t.observe(seen(Active, true), false);
        for (int i = 0; i < 40; ++i) {
            const Decision d = t.observe(seen(Disconnected), false);
            CHECK(d.leftByRemoteDesktop && !d.askReturn);
        }
    }

    // Remote Desktop moving the session to another client passes through
    // Disconnected for an instant: one tick is not enough to act.
    {
        Tracker t;
        t.observe(seen(Active, true), true);
        CHECK(!t.observe(seen(Disconnected), true).askReturn);
        CHECK(!t.observe(seen(Connected), true).askReturn);
        CHECK(!t.observe(seen(Active, true), true).askReturn);
        CHECK(!t.observe(seen(Disconnected), true).askReturn);
        CHECK(t.observe(seen(Disconnected), true).askReturn);
    }

    // Refused (somebody signed in on the PC): asked again only after
    // kRetryTicks, in case they sign out.
    {
        Tracker t;
        t.observe(seen(Active, true), true);
        t.observe(seen(Disconnected), true);
        CHECK(t.observe(seen(Disconnected), true).askReturn);
        t.returnFailed();
        int asked = 0;
        for (int i = 0; i < Tracker::kRetryTicks - 1; ++i)
            asked += t.observe(seen(Disconnected), true).askReturn ? 1 : 0;
        CHECK_EQ(asked, 0);
        CHECK(t.observe(seen(Disconnected), true).askReturn);
    }

    // The setting turned on while the session is away: acted on at once.
    {
        Tracker t;
        t.observe(seen(Active, true), true);
        t.observe(seen(Disconnected), false);
        t.observe(seen(Disconnected), false);
        CHECK(t.observe(seen(Disconnected), true).askReturn);
    }
}
