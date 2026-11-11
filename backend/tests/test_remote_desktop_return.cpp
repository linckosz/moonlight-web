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

    CHECK(consoleReturnChecks(evo()).isEmpty());

    ConsoleReturnFacts f = evo();
    f.callerIsThisExecutable = false;
    CHECK(!consoleReturnChecks(f).isEmpty());

    f = evo();
    f.callerSession = kNoSession;
    CHECK(!consoleReturnChecks(f).isEmpty());

    f = evo();
    f.callerSession = 0;
    CHECK(!consoleReturnChecks(f).isEmpty());

    f = evo();
    f.consoleSession = kNoSession;
    CHECK(!consoleReturnChecks(f).isEmpty());

    // Already there: nothing to move.
    f = evo();
    f.consoleSession = 1;
    CHECK(!consoleReturnChecks(f).isEmpty());

    // Remote Desktop still connected, or any state but Disconnected.
    for (int state : {int(Active), int(Connected), int(ConnectQuery), int(Shadow), int(Idle),
                      int(Listen), int(Reset), int(Down), int(Init), -1}) {
        f = evo();
        f.callerState = state;
        CHECK(!consoleReturnChecks(f).isEmpty());
    }

    // A second account signed in on the PC keeps its screen.
    f = evo();
    f.consoleUserPresent = true;
    CHECK(!consoleReturnChecks(f).isEmpty());

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

    SECTION("RemoteDesktopReturn — not offered in this version");

    // The card keeps its text and every request is refused, whatever the
    // tracker and the setting say: a session left by Remote Desktop, settled,
    // with the setting on, still gets no "This PC's screen" and no return.
    {
        CHECK(!kReturnOffered);
        Tracker t;
        t.observe(seen(Active, true));
        t.observe(seen(Disconnected));
        CHECK(t.observe(seen(Disconnected)));
        CHECK(t.returnRefusal(true).isEmpty()); // the tracker alone would allow it
        CHECK(!returnOffered(true, true));
        CHECK(!requestRefusal(t, true).isEmpty());
        CHECK(!returnOffered(false, true));
        CHECK(!returnOffered(true, false));
        CHECK(!requestRefusal(t, false).isEmpty());
        // The launcher refuses too, on its own side: evo's case passes every
        // check of the caller and the console, and still moves nothing.
        CHECK(consoleReturnChecks(evo()).isEmpty());
        CHECK(!consoleReturnRefusal(evo()).isEmpty());
    }

    SECTION("RemoteDesktopReturn — the server's tracker");

    // The card's app id sits between the virtual display's and the owner's
    // apps': the server never mistakes it for a display.
    CHECK(kReturnAppId > 1000 && kReturnAppId < 1100);

    // evo's case: active through Remote Desktop, then mstsc closed. The card
    // changes on the first tick; a return may be asked from the second — and
    // only asked: nothing in the tracker starts one (decision of 11/11/2026).
    {
        Tracker t;
        CHECK(!t.observe(seen(Active, true)));
        CHECK(!t.returnRefusal(true).isEmpty()); // still on Remote Desktop
        CHECK(t.observe(seen(Disconnected)));
        CHECK(!t.returnRefusal(true).isEmpty()); // just left: not settled yet
        CHECK(t.observe(seen(Disconnected)));
        CHECK(t.returnRefusal(true).isEmpty());
        // It stays possible while the session stays away...
        for (int i = 0; i < 40; ++i)
            CHECK(t.observe(seen(Disconnected)));
        CHECK(t.returnRefusal(true).isEmpty());
        // ...and back on the console it is a session like any other.
        CHECK(!t.observe(seen(Active, false)));
        CHECK(!t.returnRefusal(true).isEmpty());
    }

    // Moved but not locked, then sent away again: no return while it stays
    // away — each try would show the desktop again. Used again (Active), it
    // starts over.
    {
        Tracker t;
        t.observe(seen(Active, true));
        t.observe(seen(Disconnected));
        t.observe(seen(Disconnected));
        CHECK(t.returnRefusal(true).isEmpty());
        t.giveUp();
        for (int i = 0; i < 40; ++i) {
            CHECK(t.observe(seen(Disconnected)));
            CHECK(!t.returnRefusal(true).isEmpty());
        }
        t.observe(seen(Active, true));
        t.observe(seen(Disconnected));
        t.observe(seen(Disconnected));
        CHECK(t.returnRefusal(true).isEmpty());
    }

    // Switch user on the PC: Disconnected too, but last seen on the console.
    // Pulling it back would throw the other person off the sign-in screen.
    {
        Tracker t;
        t.observe(seen(Active, false));
        for (int i = 0; i < 40; ++i) {
            CHECK(!t.observe(seen(Disconnected)));
            CHECK(!t.returnRefusal(true).isEmpty());
        }
    }

    // The server started in a session already disconnected: nothing known of
    // how it got there, so nothing offered.
    {
        Tracker t;
        for (int i = 0; i < 5; ++i) {
            CHECK(!t.observe(seen(Disconnected)));
            CHECK(!t.returnRefusal(true).isEmpty());
        }
    }

    // The setting off (or no launcher, or the native host disabled): the card
    // still says what happened, and no return is asked. Turned on while the
    // session is away: possible at once.
    {
        Tracker t;
        t.observe(seen(Active, true));
        for (int i = 0; i < 40; ++i) {
            CHECK(t.observe(seen(Disconnected)));
            CHECK(!t.returnRefusal(false).isEmpty());
        }
        CHECK(t.returnRefusal(true).isEmpty());
    }

    // Remote Desktop moving the session to another client passes through
    // Disconnected for an instant: one tick is not enough to act, and a
    // client on its way in (Connected) resets the count.
    {
        Tracker t;
        t.observe(seen(Active, true));
        t.observe(seen(Disconnected));
        CHECK(!t.returnRefusal(true).isEmpty());
        CHECK(!t.observe(seen(Connected)));
        CHECK(!t.returnRefusal(true).isEmpty());
        t.observe(seen(Active, true));
        t.observe(seen(Disconnected));
        CHECK(!t.returnRefusal(true).isEmpty());
        t.observe(seen(Disconnected));
        CHECK(t.returnRefusal(true).isEmpty());
    }
}
