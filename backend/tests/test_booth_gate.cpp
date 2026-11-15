/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * A booth whose streams have PINs (booth_stream_pin, issue #68): who may end,
 * replace or join someone else's stream, one row per way a request reaches
 * it. Pinned down: the owner's own streams pass as without the key; a second
 * leg, a start again after a crash or a quit never pass from anyone else, PIN
 * or not; this machine always passes; a stream nobody used for the idle time
 * passes without the PIN; anything else needs the PIN of that one stream.
 * What a request is let through on is what it answers for: a take-over names
 * every slot of the one taken over, and a quit that ends the host's app
 * reaches every stream there, whatever the page names.
 */
#include "test_framework.h"

#include "server/BoothGate.h"

namespace {

using BoothGate::Action;
using BoothGate::Verdict;
using BoothGate::Why;

const StreamOwner kSam = StreamOwner::of(false, QStringLiteral("session-sam"));
const StreamOwner kAlex = StreamOwner::of(false, QStringLiteral("session-alex"));
const StreamOwner kRoby = StreamOwner::of(false, QStringLiteral("session-roby"));
const StreamOwner kHere = StreamOwner::of(true, QString());
const StreamOwner kNobody = StreamOwner::of(false, QString());

const QString kSamsStream = QStringLiteral("host-A|7|session:sam");
const QString kRobysStream = QStringLiteral("host-A|7|session:roby");

constexpr qint64 kNow = 1'800'000'000'000;
constexpr qint64 kMinute = 60 * 1000;

/// Sam streams host A (slot 0 and the standby leg of a quality switch); Alex asks.
BoothGate::Input samStreams(Action action, const StreamOwner& requester = kAlex)
{
    BoothGate::Input in;
    in.keyOn = true;
    in.requester = requester;
    in.action = action;
    in.inReach = {{kSam, kSamsStream}, {kSam, kSamsStream}};
    in.nowMs = kNow;
    in.lastActivityMs = kNow - kMinute;
    in.idleMinutes = 15;
    return in;
}

const Action kAll[] = {Action::Fresh,     Action::Fallback,    Action::Standby,
                       Action::Reconnect, Action::StopSession, Action::Quit};

void testTheKeyOff()
{
    SECTION("BoothGate: the key off, everything as before");
    for (const Action a : kAll) {
        BoothGate::Input in = samStreams(a);
        in.keyOn = false;
        const BoothGate::Decision d = BoothGate::decide(in);
        CHECK(d.verdict == Verdict::Allow);
        CHECK(d.why == Why::KeyOff);
        CHECK(d.code().isEmpty());
        CHECK_EQ(d.status(a), 200);
    }
}

void testNothingInTheWay()
{
    SECTION("BoothGate: no stream in reach, or only the caller's own");
    for (const Action a : kAll) {
        BoothGate::Input in = samStreams(a);
        in.inReach.clear();
        CHECK(BoothGate::decide(in).why == Why::NothingInReach);

        // Sam's own: a relaunch, a quality switch, a fallback, a start again
        // after a crash, a stop, a quit: all as without the key.
        const BoothGate::Decision own = BoothGate::decide(samStreams(a, kSam));
        CHECK(own.verdict == Verdict::Allow);
        CHECK(own.why == Why::SameOwner);
    }
    // This machine's own stream, asked from this machine.
    BoothGate::Input here = samStreams(Action::Standby, kHere);
    here.inReach = {{kHere, QStringLiteral("host-A|7|this-machine:")}};
    CHECK(BoothGate::decide(here).why == Why::SameOwner);
}

void testAFreshStartNeedsThePin()
{
    SECTION("BoothGate: another's start, the same app or another, needs the PIN");
    for (const Action a : {Action::Fresh, Action::Fallback, Action::StopSession}) {
        const BoothGate::Decision d = BoothGate::decide(samStreams(a));
        CHECK(d.verdict == Verdict::NeedPin);
        CHECK(d.why == Why::PinRequired);
        CHECK(d.streamKey == kSamsStream);
        CHECK(d.code() == QStringLiteral("booth_pin_required"));
        CHECK_EQ(d.status(a), 409);
    }
    // Sam's stream among Alex's own: Sam's decides, and its PIN is the one.
    BoothGate::Input mixed = samStreams(Action::Fresh);
    mixed.inReach = {{kAlex, QStringLiteral("host-A|7|session:alex")}, {kSam, kSamsStream}};
    const BoothGate::Decision d = BoothGate::decide(mixed);
    CHECK(d.verdict == Verdict::NeedPin);
    CHECK(d.streamKey == kSamsStream);
}

void testSecondLegsAreTheOwnersOnly()
{
    SECTION("BoothGate: a second leg or a start again after a crash, never from another");
    const BoothGate::Decision standby = BoothGate::decide(samStreams(Action::Standby));
    CHECK(standby.verdict == Verdict::Refuse);
    CHECK(standby.code() == QStringLiteral("booth_owner_only"));
    CHECK_EQ(standby.status(Action::Standby), 409);
    CHECK(standby.streamKey.isEmpty());

    const BoothGate::Decision again = BoothGate::decide(samStreams(Action::Reconnect));
    CHECK(again.verdict == Verdict::Refuse);
    CHECK(again.code() == QStringLiteral("reconnect_refused"));
    CHECK_EQ(again.status(Action::Reconnect), 409);

    // Idle changes nothing, and neither does this machine: a second leg is
    // never a newcomer.
    for (const Action a : {Action::Standby, Action::Reconnect}) {
        BoothGate::Input in = samStreams(a);
        in.lastActivityMs = kNow - 60 * kMinute;
        CHECK(BoothGate::decide(in).verdict == Verdict::Refuse);
        CHECK(BoothGate::decide(samStreams(a, kHere)).verdict == Verdict::Refuse);
    }
}

void testAQuitIsOnesOwn()
{
    SECTION("BoothGate: a quit reaches the caller's own streams only");
    const BoothGate::Decision d = BoothGate::decide(samStreams(Action::Quit));
    CHECK(d.verdict == Verdict::Refuse);
    CHECK(d.code() == QStringLiteral("booth_owner_only"));
    CHECK_EQ(d.status(Action::Quit), 403);
    BoothGate::Input idle = samStreams(Action::Quit);
    idle.lastActivityMs = kNow - 60 * kMinute;
    CHECK(BoothGate::decide(idle).verdict == Verdict::Refuse);
    CHECK(BoothGate::decide(samStreams(Action::Quit, kHere)).verdict == Verdict::Allow);
}

void testThisMachineAlwaysPasses()
{
    SECTION("BoothGate: this machine takes its screen back, no PIN");
    for (const Action a : {Action::Fresh, Action::Fallback, Action::StopSession, Action::Quit}) {
        const BoothGate::Decision d = BoothGate::decide(samStreams(a, kHere));
        CHECK(d.verdict == Verdict::Allow);
        CHECK(d.why == Why::ThisMachine);
    }
    // A host-key session over the public domain is a session like any other:
    // nothing makes it this machine.
    const StreamOwner hostKey = StreamOwner::of(false, QStringLiteral("host-key-session"));
    CHECK(BoothGate::decide(samStreams(Action::Fresh, hostKey)).verdict == Verdict::NeedPin);
}

void testIdleStreams()
{
    SECTION("BoothGate: a stream nobody used for the idle time is taken without the PIN");
    BoothGate::Input in = samStreams(Action::Fresh);
    in.lastActivityMs = kNow - 15 * kMinute;
    BoothGate::Decision d = BoothGate::decide(in);
    CHECK(d.verdict == Verdict::AllowIdle);
    CHECK(d.why == Why::Idle);
    CHECK(d.allowed());
    CHECK(d.streamKey == kSamsStream);
    CHECK(d.code().isEmpty());

    in.action = Action::StopSession;
    CHECK(BoothGate::decide(in).verdict == Verdict::AllowIdle);

    // A second short of it, the idle time off, or a last use nobody knows:
    // the PIN.
    in.action = Action::Fresh;
    in.lastActivityMs = kNow - 15 * kMinute + 1000;
    CHECK(BoothGate::decide(in).verdict == Verdict::NeedPin);
    in.lastActivityMs = kNow - 600 * kMinute;
    in.idleMinutes = 0;
    CHECK(BoothGate::decide(in).verdict == Verdict::NeedPin);
    in.idleMinutes = 15;
    in.lastActivityMs = 0;
    CHECK(BoothGate::decide(in).verdict == Verdict::NeedPin);
    in.lastActivityMs = -1;
    CHECK(BoothGate::decide(in).verdict == Verdict::NeedPin);

    CHECK(BoothGate::idle(kNow, kNow - 15 * kMinute, 15));
    CHECK(!BoothGate::idle(kNow, kNow - 15 * kMinute + 1, 15));
    CHECK(!BoothGate::idle(kNow, kNow - 300 * kMinute, 0));
    CHECK(!BoothGate::idle(kNow, 0, 15));
}

void testNobodyKnown()
{
    SECTION("BoothGate: nobody known on either side, only this machine passes");
    // A stream whose start named nobody: no PIN, no idle, this machine only.
    BoothGate::Input in = samStreams(Action::Fresh);
    in.inReach = {{kNobody, QStringLiteral("host-A|7|unknown:x")}};
    CHECK(BoothGate::decide(in).why == Why::NobodyKnown);
    in.lastActivityMs = kNow - 60 * kMinute;
    CHECK(BoothGate::decide(in).verdict == Verdict::Refuse);
    in.requester = kHere;
    CHECK(BoothGate::decide(in).verdict == Verdict::Allow);
    // Two such starts are two strangers, never one owner.
    in.requester = kNobody;
    CHECK(BoothGate::decide(in).verdict == Verdict::Refuse);

    // A caller nobody can name, on Sam's stream, idle or not.
    for (const Action a : kAll) {
        BoothGate::Input asked = samStreams(a, kNobody);
        asked.lastActivityMs = kNow - 60 * kMinute;
        CHECK(BoothGate::decide(asked).verdict == Verdict::Refuse);
    }
    // A stream with no key to keep a PIN under opens to nobody's PIN.
    BoothGate::Input keyless = samStreams(Action::Fresh);
    keyless.inReach = {{kSam, QString()}};
    CHECK(BoothGate::decide(keyless).verdict == Verdict::Refuse);
}

void testOnePinOpensOneStream()
{
    SECTION("BoothGate: streams of two others are not opened by one PIN");
    BoothGate::Input in = samStreams(Action::Fresh);
    in.inReach = {{kSam, kSamsStream}, {kRoby, kRobysStream}};
    const BoothGate::Decision d = BoothGate::decide(in);
    CHECK(d.verdict == Verdict::Refuse);
    CHECK(d.why == Why::SeveralOwners);
    CHECK(d.code() == QStringLiteral("booth_owner_only"));
    in.lastActivityMs = kNow - 60 * kMinute;
    CHECK(BoothGate::decide(in).verdict == Verdict::Refuse);
    in.requester = kHere;
    CHECK(BoothGate::decide(in).verdict == Verdict::Allow);
}

void testATakeOverTakesAllOfIt()
{
    SECTION("BoothGate: a take-over names every slot of the one taken over");
    // Sam streams in slot 0, the standby leg of a quality switch in slot 1,
    // and a slot of their own beside them (7); a start of theirs is on its way.
    BoothGate::Input in = samStreams(Action::Fresh);
    in.inReach = {{kSam, kSamsStream, 0},
                  {kSam, kSamsStream, 1},
                  {kSam, kSamsStream, 7},
                  {kSam, kSamsStream, 7},
                  {kSam, kSamsStream, -1}};
    BoothGate::Decision d = BoothGate::decide(in);
    CHECK(d.verdict == Verdict::NeedPin);
    CHECK(d.othersSlots == QList<int>({0, 1, 7}));
    // The same slots, idle or from this machine.
    in.lastActivityMs = kNow - 60 * kMinute;
    d = BoothGate::decide(in);
    CHECK(d.verdict == Verdict::AllowIdle);
    CHECK(d.othersSlots == QList<int>({0, 1, 7}));
    in.requester = kHere;
    d = BoothGate::decide(in);
    CHECK(d.verdict == Verdict::Allow);
    CHECK(d.othersSlots == QList<int>({0, 1, 7}));

    // Alex's own slot beside Sam's is never one to end.
    in = samStreams(Action::Fresh);
    in.inReach = {{kAlex, QStringLiteral("host-A|7|session:alex"), 6}, {kSam, kSamsStream, 0}};
    CHECK(BoothGate::decide(in).othersSlots == QList<int>({0}));
    in.inReach = {{kAlex, QStringLiteral("host-A|7|session:alex"), 6}};
    CHECK(BoothGate::decide(in).othersSlots.isEmpty());

    // Once Sam's slots ended, Alex's next leg is Alex's own business.
    BoothGate::Input next = samStreams(Action::Standby);
    next.inReach = {{kAlex, QStringLiteral("host-A|9|session:alex"), 0}};
    CHECK(BoothGate::decide(next).why == Why::SameOwner);
    // While one of Sam's is left, it is not.
    next.inReach.append({kSam, kSamsStream, 7});
    CHECK(BoothGate::decide(next).verdict == Verdict::Refuse);
}

void testWhatAQuitReaches()
{
    SECTION("BoothGate: a quit that ends the host's app reaches every stream there");
    const QString samsPage = QStringLiteral("5A3F00C1D2E3F405");
    const QString anyPage = QStringLiteral("0123456789ABCDEF");
    // A host-wide cancel: whatever the page names, and naming nothing.
    for (const QString& named : {anyPage, samsPage, QString()})
        CHECK(BoothGate::quitReaches(true, named, samsPage));
    // A retire, or a quit that ends no app: the browser it names.
    CHECK(BoothGate::quitReaches(false, samsPage, samsPage));
    CHECK(!BoothGate::quitReaches(false, anyPage, samsPage));
    CHECK(BoothGate::quitReaches(false, QString(), samsPage));
    CHECK(BoothGate::quitReaches(false, anyPage, QString()));

    // Alex stops with an id of their own while Sam streams: the cancel would
    // end Sam's stream, so the whole quit is refused.
    auto quit = [&](bool endsTheApp, const QString& named, const StreamOwner& by) {
        BoothGate::Input in = samStreams(Action::Quit, by);
        in.inReach.clear();
        if (BoothGate::quitReaches(endsTheApp, named, samsPage))
            in.inReach = {{kSam, kSamsStream, 0}};
        return BoothGate::decide(in);
    };
    BoothGate::Decision d = quit(true, anyPage, kAlex);
    CHECK(d.verdict == Verdict::Refuse);
    CHECK(d.code() == QStringLiteral("booth_owner_only"));
    CHECK_EQ(d.status(Action::Quit), 403);
    // A retire with that id touches nothing of Sam's, and goes on.
    d = quit(false, anyPage, kAlex);
    CHECK(d.verdict == Verdict::Allow);
    CHECK(d.why == Why::NothingInReach);
    // Sam's own Stop, and this machine's, as before.
    CHECK(quit(true, samsPage, kSam).why == Why::SameOwner);
    CHECK(quit(true, anyPage, kHere).why == Why::ThisMachine);
}

void testOwnersAreToldApartBySession()
{
    SECTION("BoothGate: owners told apart by their session, never by its absence");
    CHECK(!kSam.sameAs(kAlex));
    CHECK(!kHere.sameAs(kNobody));
    CHECK(!kNobody.sameAs(kNobody));
    CHECK(kHere.sameAs(StreamOwner::of(true, QStringLiteral("a cookie this machine sent"))));
    BoothGate::Input in =
        samStreams(Action::Fresh, StreamOwner::of(false, QStringLiteral("session-sa")));
    CHECK(BoothGate::decide(in).verdict == Verdict::NeedPin);
}

} // namespace

void run_booth_gate_tests()
{
    testTheKeyOff();
    testNothingInTheWay();
    testAFreshStartNeedsThePin();
    testSecondLegsAreTheOwnersOnly();
    testAQuitIsOnesOwn();
    testThisMachineAlwaysPasses();
    testIdleStreams();
    testNobodyKnown();
    testOnePinOpensOneStream();
    testATakeOverTakesAllOfIt();
    testWhatAQuitReaches();
    testOwnersAreToldApartBySession();
}
