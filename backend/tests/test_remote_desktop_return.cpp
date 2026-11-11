/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "platform/SignInScreen.h"
#include "streaming/RemoteDesktopReturnPolicy.h"
#include "streaming/WorkerService.h"

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

/// What the launcher sees when the server of a session Remote Desktop left
/// asks for the sign-in screen: its session 1 is Disconnected, the console is
/// an empty session 10 at the sign-in screen, and no sign-in worker runs.
SignInFacts signInAsked()
{
    SignInFacts f;
    f.callerIsThisExecutable = true;
    f.callerSession = 1;
    f.callerState = Disconnected;
    f.consoleSession = 10;
    f.consoleUserPresent = false;
    f.signInWorkerAlive = false;
    return f;
}

/// A session last seen active through Remote Desktop, then Disconnected on
/// @p ticks ticks.
Tracker leftFor(int ticks)
{
    Tracker t;
    t.observe(seen(Active, true));
    for (int i = 0; i < ticks; ++i)
        t.observe(seen(Disconnected));
    return t;
}

/// The access mask the control pipe's DACL gives the interactive user, read
/// from its SDDL; 0 when there is no such entry.
quint32 interactiveUserMask(const QString& sddl)
{
    const int end = sddl.indexOf(QStringLiteral(";;;IU)"));
    if (end < 0) return 0;
    const int start = sddl.lastIndexOf(QStringLiteral("(A;;"), end);
    if (start < 0) return 0;
    bool ok = false;
    const quint32 mask = sddl.mid(start + 4, end - start - 4).toUInt(&ok, 16);
    return ok ? mask : 0;
}

/// The launcher's control pipe: the user may talk to it and nothing more, and
/// a caller speaks only to the pipe the service itself serves.
void run_control_pipe_tests()
{
    SECTION("WorkerService — what the interactive user may do on the control pipe");
    {
        const QString sddl = QString::fromLatin1(WorkerService::kControlPipeSddl);
        // Protected: nothing inherited widens it.
        CHECK(sddl.startsWith(QStringLiteral("D:P")));
        CHECK(sddl.contains(QStringLiteral("(A;;GA;;;SY)")));
        CHECK(sddl.contains(QStringLiteral("(A;;GA;;;BA)")));
        // Nobody else has an entry: no Everyone, no Authenticated Users.
        CHECK_EQ(sddl.count(QLatin1Char('(')), 3);

        const quint32 mask = interactiveUserMask(sddl);
        CHECK_EQ(mask, 0x0012019Bu);
        // FILE_CREATE_PIPE_INSTANCE (FILE_APPEND_DATA): only the service makes
        // instances of its pipe.
        CHECK((mask & 0x00000004u) == 0);
        // FILE_GENERIC_READ (what GENERIC_READ maps to) and FILE_WRITE_DATA:
        // a request written, its answer read.
        CHECK((mask & 0x00120089u) == 0x00120089u);
        CHECK((mask & 0x00000002u) != 0);
        // Nothing that changes the pipe's owner or its descriptor.
        CHECK((mask & 0x000C0000u) == 0); // WRITE_DAC, WRITE_OWNER
        CHECK((mask & 0x00010000u) == 0); // DELETE

        // What a caller asks for when it opens the pipe: read, and write the
        // data. Never GENERIC_WRITE, which asks for the instance right too.
        CHECK_EQ(WorkerService::kControlPipeClientAccess, 0x80000002u);
        CHECK((WorkerService::kControlPipeClientAccess & 0x40000000u) == 0);
        CHECK((WorkerService::kControlPipeClientAccess & 0x00000004u) == 0);
    }

    SECTION("WorkerService — a caller speaks only to the service's own pipe");
    CHECK(WorkerService::controlPipeServerIsService(4242, 4242));
    // Served by another process: the service stopped, and something else made
    // a pipe of that name.
    CHECK(!WorkerService::controlPipeServerIsService(4242, 1717));
    // The service is not running, or could not be asked.
    CHECK(!WorkerService::controlPipeServerIsService(4242, 0));
    CHECK(!WorkerService::controlPipeServerIsService(0, 0));
}

/// The console's sign-in screen, streamed by a SYSTEM worker on a console
/// nobody is signed in on: the launcher's narrow exception, who may ask for
/// it, and the worker's per-picture and per-input rule.
void run_sign_in_screen_tests()
{
    SECTION("Sign-in screen — not offered in this version, by anyone");
    if (!kSignInOffered) {
        CHECK_EQ(signInLaunchRefusal(signInAsked()),
                 QStringLiteral("this version does not offer it"));
        CHECK(!signInOffered(true, true));
        const Tracker t = leftFor(5);
        CHECK_EQ(signInRequestRefusal(t, true, false),
                 QStringLiteral("this version does not offer it"));
    } else {
        CHECK(signInLaunchRefusal(signInAsked()).isEmpty());
        CHECK(signInOffered(true, true));
    }
    CHECK(!signInOffered(false, true));
    CHECK(!signInOffered(true, false));
    CHECK(kSignInAppId != kReturnAppId);

    SECTION("Sign-in screen — the launcher's checks");
    CHECK(signInLaunchChecks(signInAsked()).isEmpty());
    {
        SignInFacts f = signInAsked();
        f.callerIsThisExecutable = false;
        CHECK(!signInLaunchChecks(f).isEmpty());

        f = signInAsked();
        f.callerSession = kNoSession;
        CHECK(!signInLaunchChecks(f).isEmpty());

        f = signInAsked();
        f.callerSession = 0;
        CHECK(!signInLaunchChecks(f).isEmpty());

        // The caller's own session is in use (on a screen or through Remote
        // Desktop), or on its way back: no exception for it.
        for (int state :
             {static_cast<int>(Active), static_cast<int>(Connected), static_cast<int>(ConnectQuery),
              static_cast<int>(Shadow), static_cast<int>(Idle), static_cast<int>(Listen),
              static_cast<int>(Reset), static_cast<int>(Down), static_cast<int>(Init), -1}) {
            f = signInAsked();
            f.callerState = state;
            CHECK(!signInLaunchChecks(f).isEmpty());
        }

        f = signInAsked();
        f.consoleSession = kNoSession;
        CHECK(!signInLaunchChecks(f).isEmpty());

        f = signInAsked();
        f.consoleSession = 0;
        CHECK(!signInLaunchChecks(f).isEmpty());

        f = signInAsked();
        f.consoleSession = f.callerSession;
        CHECK(!signInLaunchChecks(f).isEmpty());

        // Somebody signed in on the PC, or Windows would not say: the
        // console is theirs, and never streamed from here.
        f = signInAsked();
        f.consoleUserPresent = true;
        CHECK(!signInLaunchChecks(f).isEmpty());

        // One sign-in screen worker at a time.
        f = signInAsked();
        f.signInWorkerAlive = true;
        CHECK(!signInLaunchChecks(f).isEmpty());

        // Every unknown reads as a refusal.
        CHECK(!signInLaunchChecks(SignInFacts{}).isEmpty());
    }

    SECTION("Sign-in screen — who may ask the server for it");
    {
        // Left by Remote Desktop and settled, the native host and the
        // launcher there: yes, from any paired browser but a kiosk.
        const Tracker left = leftFor(Tracker::kSettleTicks);
        CHECK(signInRequestChecks(left, true, false).isEmpty());
        CHECK_EQ(signInRequestChecks(left, true, true), QStringLiteral("not from a kiosk"));
        CHECK(!signInRequestChecks(left, false, false).isEmpty());
        // Only just left: Remote Desktop may be moving it to another client.
        CHECK(!signInRequestChecks(leftFor(1), true, false).isEmpty());
        // Switched away from on the PC (Switch user), or never seen in use:
        // not left by Remote Desktop, nothing offered.
        Tracker local;
        local.observe(seen(Active, false));
        for (int i = 0; i < 5; ++i)
            local.observe(seen(Disconnected));
        CHECK(!signInRequestChecks(local, true, false).isEmpty());
        CHECK(!signInRequestChecks(Tracker{}, true, false).isEmpty());
        // A return given up on does not hold the sign-in screen back:
        // nothing is moved there.
        Tracker gaveUp = leftFor(Tracker::kSettleTicks);
        gaveUp.giveUp();
        CHECK(signInRequestChecks(gaveUp, true, false).isEmpty());
        // Back on a screen: nothing to offer.
        Tracker back = leftFor(Tracker::kSettleTicks);
        back.observe(seen(Active, false));
        CHECK(!signInRequestChecks(back, true, false).isEmpty());
    }

    SECTION("Sign-in screen — never a guest's");
    CHECK(!guestMayStream(true, kSignInAppId));
    CHECK(guestMayStream(true, 1));
    CHECK(guestMayStream(true, kReturnAppId + 100));
    // Another host's app of the same number is that host's business.
    CHECK(guestMayStream(false, kSignInAppId));

    SECTION("Sign-in screen — what the worker sends and injects");
    {
        using mw::native::platform::isSignInDesktop;
        using mw::native::platform::SignInLook;
        using mw::native::platform::signInRefusal;
        CHECK(isSignInDesktop("Winlogon"));
        CHECK(isSignInDesktop("winlogon"));
        CHECK(isSignInDesktop("WINLOGON"));
        CHECK(!isSignInDesktop("Default"));
        CHECK(!isSignInDesktop(""));
        CHECK(!isSignInDesktop("Winlogon2"));
        CHECK(!isSignInDesktop("Screen-saver"));

        SignInLook look;
        look.inputDesktop = "Winlogon";
        look.threadDesktop = "Winlogon";
        look.sessionHasUser = false;
        CHECK(signInRefusal(look, true).empty());
        CHECK(signInRefusal(look, false).empty());

        // Somebody signed in on the session: nothing more, picture or input.
        SignInLook signedIn = look;
        signedIn.sessionHasUser = true;
        CHECK(!signInRefusal(signedIn, true).empty());
        CHECK(!signInRefusal(signedIn, false).empty());

        // Their desktop has the screen: nothing more either.
        SignInLook theirDesktop = look;
        theirDesktop.inputDesktop = "Default";
        CHECK(!signInRefusal(theirDesktop, true).empty());
        CHECK(!signInRefusal(theirDesktop, false).empty());

        // The screen could not be read: a doubt stops it.
        SignInLook unread = look;
        unread.inputDesktop.clear();
        CHECK(!signInRefusal(unread, true).empty());
        CHECK(!signInRefusal(unread, false).empty());

        // A picture is copied from the capturing thread's own desktop: that
        // one has to be the sign-in screen too. An input does not look at it.
        SignInLook thread = look;
        thread.threadDesktop = "Default";
        CHECK(!signInRefusal(thread, true).empty());
        CHECK(signInRefusal(thread, false).empty());
        thread.threadDesktop.clear();
        CHECK(!signInRefusal(thread, true).empty());

        // A look that read nothing at all is no sign-in screen.
        CHECK(!signInRefusal(SignInLook{}, true).empty());
        CHECK(!signInRefusal(SignInLook{}, false).empty());
    }
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

    run_sign_in_screen_tests();
    run_control_pipe_tests();
}
