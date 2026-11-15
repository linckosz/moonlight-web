/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * A booth's stream PINs (issue #68): what a PIN is, when it changes, and the two
 * buckets every try is held to. Guessing must never end a stream, only move the
 * PIN; a shut bucket must never be tried through; and nothing is ever matched
 * by a PIN that is only close.
 */
#include "test_framework.h"

#include "server/BoothPin.h"

#include <QSet>
#include <QString>
#include <QStringList>

namespace {

const QString kStream = QStringLiteral("host-uuid-A");
const QString kOther = QStringLiteral("host-uuid-B/7");

/// A clock the test moves by hand.
struct Clock
{
    qint64 now = 1'800'000'000;
};

/// PINs handed out in order, then the default source's shape.
struct Script
{
    QStringList next;
    int at = 0;
    QString operator()() { return at < next.size() ? next.at(at++) : QStringLiteral("000000"); }
};

void testWhatAPinIs()
{
    SECTION("BoothPin: six digits, one per stream, gone with it");
    BoothPin pins;
    CHECK(!pins.has(kStream));
    CHECK(pins.read(kStream).isEmpty());
    CHECK(pins.rotate(kStream, BoothPin::Reason::Asked).isEmpty());

    const QString pin = pins.mint(kStream);
    CHECK_EQ(pin.size(), BoothPin::kDigits);
    bool digits = true;
    for (const QChar c : pin)
        digits = digits && c.isDigit() && c.unicode() < 128;
    CHECK(digits);
    CHECK(pins.has(kStream));
    CHECK(pins.read(kStream) == pin);
    CHECK_EQ(pins.generation(kStream), quint64(0));
    CHECK(pins.lastReason(kStream) == BoothPin::Reason::Minted);

    // Another stream has its own; forgetting one leaves the other.
    const QString other = pins.mint(kOther);
    CHECK(pins.read(kOther) == other);
    pins.forget(kStream);
    CHECK(!pins.has(kStream));
    CHECK(pins.read(kStream).isEmpty());
    CHECK(pins.read(kOther) == other);

    // The default source spreads: a hundred mints are not one value.
    QSet<QString> seen;
    for (int i = 0; i < 100; ++i)
        seen.insert(pins.mint(kOther));
    CHECK(seen.size() > 90);
}

void testAChangeIsAChange()
{
    SECTION("BoothPin: a new PIN never repeats the one it replaces");
    BoothPin pins;
    Script script{{QStringLiteral("111111"), QStringLiteral("111111"), QStringLiteral("111111"),
                   QStringLiteral("222222")}};
    pins.setGeneratorForTesting([&script]() { return script(); });
    CHECK(pins.mint(kStream) == QStringLiteral("111111"));
    CHECK(pins.rotate(kStream, BoothPin::Reason::Used) == QStringLiteral("222222"));
    CHECK_EQ(pins.generation(kStream), quint64(1));
    CHECK(pins.lastReason(kStream) == BoothPin::Reason::Used);

    for (const BoothPin::Reason why :
         {BoothPin::Reason::Departed, BoothPin::Reason::Asked, BoothPin::Reason::Used}) {
        const quint64 before = pins.generation(kStream);
        pins.setGeneratorForTesting([]() { return QStringLiteral("333333"); });
        pins.rotate(kStream, why);
        CHECK_EQ(pins.generation(kStream), before + 1);
        CHECK(pins.lastReason(kStream) == why);
    }
}

void testTheRightPin()
{
    SECTION("BoothPin: the right PIN, and only it");
    BoothPin pins;
    Clock clock;
    pins.setClockForTesting([&clock]() { return clock.now; });
    pins.setGeneratorForTesting([]() { return QStringLiteral("482913"); });
    pins.mint(kStream);

    // Spaces are how the page shows it; they are not part of it.
    BoothPin::Outcome o = pins.check(kStream, QStringLiteral("482 913"), "s1", "10.0.0.5");
    CHECK(o.result == BoothPin::Result::Ok);
    CHECK(!o.rotated);
    // A use is the caller's to report; checking alone moves nothing.
    CHECK(pins.read(kStream) == QStringLiteral("482913"));
    CHECK(pins.check(kStream, QStringLiteral("482913"), "s1", "10.0.0.5").result ==
          BoothPin::Result::Ok);

    // Close is not equal: a prefix, a longer one, another stream's, nothing.
    for (const char* near : {"48291", "4829130", "482914", "", " ", "48291 3x"}) {
        BoothPin fresh;
        fresh.setGeneratorForTesting([]() { return QStringLiteral("482913"); });
        fresh.mint(kStream);
        CHECK(fresh.check(kStream, QString::fromLatin1(near), "s1", "10.0.0.5").result ==
              BoothPin::Result::Wrong);
    }

    // No PIN for a stream that has none: nothing tried, nothing counted.
    for (int i = 0; i < 20; ++i)
        CHECK(pins.check(kOther, QStringLiteral("482913"), "s1", "10.0.0.5").result ==
              BoothPin::Result::NoStream);
    CHECK(pins.check(kStream, QStringLiteral("482913"), "s1", "10.0.0.5").result ==
          BoothPin::Result::Ok);
}

void testGuessingMovesThePin()
{
    SECTION("BoothPin: wrong tries move the PIN, whoever makes them");
    BoothPin pins;
    Clock clock;
    pins.setClockForTesting([&clock]() { return clock.now; });
    Script script{{QStringLiteral("482913"), QStringLiteral("730158")}};
    pins.setGeneratorForTesting([&script]() { return script(); });
    pins.mint(kStream);

    // Five different sessions from five addresses, one wrong try each: no
    // bucket shuts, and the fifth try is the one that moves the PIN.
    for (int i = 0; i < BoothPin::kFailuresBeforeRotation; ++i) {
        const BoothPin::Outcome o =
            pins.check(kStream, QStringLiteral("000001"), QStringLiteral("session-%1").arg(i),
                       QStringLiteral("10.0.0.%1").arg(10 + i));
        CHECK(o.result == BoothPin::Result::Wrong);
        CHECK_EQ(o.lockoutSeconds, 0);
        CHECK_EQ(o.rotated, i == BoothPin::kFailuresBeforeRotation - 1);
    }
    CHECK(pins.read(kStream) == QStringLiteral("730158"));
    CHECK(pins.lastReason(kStream) == BoothPin::Reason::Failures);
    CHECK_EQ(pins.generation(kStream), quint64(1));
    // The old PIN is dead, the new one works, and the stream is still there.
    CHECK(pins.check(kStream, QStringLiteral("482913"), "fresh", "10.0.1.1").result ==
          BoothPin::Result::Wrong);
    CHECK(pins.has(kStream));
    CHECK(pins.check(kStream, QStringLiteral("730158"), "fresh2", "10.0.1.2").result ==
          BoothPin::Result::Ok);

    // A rotation for any reason starts the count again.
    BoothPin again;
    again.setClockForTesting([&clock]() { return clock.now; });
    again.mint(kStream);
    for (int i = 0; i < BoothPin::kFailuresBeforeRotation - 1; ++i)
        again.check(kStream, QStringLiteral("x"), QStringLiteral("a%1").arg(i),
                    QStringLiteral("10.1.0.%1").arg(i));
    again.rotate(kStream, BoothPin::Reason::Used);
    const quint64 g = again.generation(kStream);
    const BoothPin::Outcome o = again.check(kStream, QStringLiteral("x"), "b", "10.2.0.1");
    CHECK(!o.rotated);
    CHECK_EQ(again.generation(kStream), g);
}

void testTheSessionsBucket()
{
    SECTION("BoothPin: one session's bucket shuts, whichever address it tries from");
    BoothPin pins;
    Clock clock;
    pins.setClockForTesting([&clock]() { return clock.now; });
    pins.setGeneratorForTesting([]() { return QStringLiteral("482913"); });
    pins.mint(kStream);

    BoothPin::Outcome o;
    o = pins.check(kStream, "000000", "mallory", "10.0.0.1");
    CHECK(o.result == BoothPin::Result::Wrong);
    CHECK_EQ(o.remainingAttempts, 2);
    o = pins.check(kStream, "000000", "mallory", "10.0.0.2");
    CHECK_EQ(o.remainingAttempts, 1);
    o = pins.check(kStream, "000000", "mallory", "10.0.0.3");
    CHECK(o.result == BoothPin::Result::Wrong);
    CHECK_EQ(o.lockoutSeconds, 30);

    // Shut: even the right PIN from a new address is not tried, and the try
    // counts towards nothing.
    const quint64 g = pins.generation(kStream);
    for (int i = 0; i < 10; ++i) {
        o = pins.check(kStream, "482913", "mallory", QStringLiteral("10.9.0.%1").arg(i));
        CHECK(o.result == BoothPin::Result::RateLimited);
        CHECK(o.lockoutSeconds > 0);
    }
    CHECK_EQ(pins.generation(kStream), g);
    // Another session from another address is not held by it.
    CHECK(pins.check(kStream, "482913", "alice", "10.0.5.5").result == BoothPin::Result::Ok);

    // Open again once the lockout ran out; the ladder goes on from where it was,
    // and past three every failure shuts it again.
    clock.now += 31;
    o = pins.check(kStream, "000000", "mallory", "10.0.0.4");
    CHECK(o.result == BoothPin::Result::Wrong);
    CHECK_EQ(o.lockoutSeconds, 30); // the fourth
    CHECK(pins.check(kStream, "000000", "mallory", "10.0.0.6").result ==
          BoothPin::Result::RateLimited);
    clock.now += 31;
    o = pins.check(kStream, "000000", "mallory", "10.0.0.6");
    CHECK(o.result == BoothPin::Result::Wrong);
    CHECK_EQ(o.lockoutSeconds, 120); // the fifth
    // Past five, every failure shuts it again: wait each lockout out.
    for (int i = 0; i < 5; ++i) {
        clock.now += 121;
        o = pins.check(kStream, "000000", "mallory", QStringLiteral("10.3.0.%1").arg(i));
        CHECK(o.result == BoothPin::Result::Wrong);
    }
    CHECK_EQ(o.lockoutSeconds, 600); // the tenth
}

void testTheAddresssBucket()
{
    SECTION("BoothPin: one address's bucket shuts, whichever session tries from it");
    BoothPin pins;
    Clock clock;
    pins.setClockForTesting([&clock]() { return clock.now; });
    pins.setGeneratorForTesting([]() { return QStringLiteral("482913"); });
    pins.mint(kStream);
    for (int i = 0; i < 3; ++i)
        pins.check(kStream, "000000", QStringLiteral("session-%1").arg(i), "2001:db8:1:2::/64");
    BoothPin::Outcome o = pins.check(kStream, "482913", "session-9", "2001:db8:1:2::/64");
    CHECK(o.result == BoothPin::Result::RateLimited);
    CHECK(pins.check(kStream, "482913", "session-9", "2001:db8:9:9::/64").result ==
          BoothPin::Result::Ok);

    // The right PIN empties nothing: the next wrong one from that address
    // shuts it again, as the ladder had it.
    clock.now += 31;
    CHECK(pins.check(kStream, "482913", "session-1", "2001:db8:1:2::/64").result ==
          BoothPin::Result::Ok);
    o = pins.check(kStream, "000000", "session-1", "2001:db8:1:2::/64");
    CHECK(o.result == BoothPin::Result::Wrong);
    CHECK_EQ(o.lockoutSeconds, 30);
}

void testOneStreamsPinBuysNoTriesAtAnother()
{
    SECTION("BoothPin: knowing one stream's PIN buys no tries at another's");
    const QString a = QStringLiteral("stream-A");
    const QString b = QStringLiteral("stream-B");

    // One session, one address: a member of A, who reads A's PIN, guessing B's.
    {
        BoothPin pins;
        Clock clock;
        pins.setClockForTesting([&clock]() { return clock.now; });
        Script script{{QStringLiteral("111111"), QStringLiteral("222222")}};
        pins.setGeneratorForTesting([&script]() { return script(); });
        pins.mint(a);
        pins.mint(b);
        CHECK(pins.check(b, "000001", "member", "10.0.0.7").result == BoothPin::Result::Wrong);
        CHECK(pins.check(b, "000002", "member", "10.0.0.7").result == BoothPin::Result::Wrong);
        CHECK(pins.check(a, "111111", "member", "10.0.0.7").result == BoothPin::Result::Ok);
        const BoothPin::Outcome o = pins.check(b, "000003", "member", "10.0.0.7");
        CHECK(o.result == BoothPin::Result::Wrong);
        CHECK_EQ(o.lockoutSeconds, 30); // the third wrong one, tier 1
        CHECK(pins.check(b, "222222", "member", "10.0.0.7").result ==
              BoothPin::Result::RateLimited);
    }
    // Two sessions on one address: the address's bucket is not reopened either.
    {
        BoothPin pins;
        Clock clock;
        pins.setClockForTesting([&clock]() { return clock.now; });
        Script script{{QStringLiteral("111111"), QStringLiteral("222222")}};
        pins.setGeneratorForTesting([&script]() { return script(); });
        pins.mint(a);
        pins.mint(b);
        CHECK(pins.check(b, "000001", "one", "10.0.0.8").result == BoothPin::Result::Wrong);
        CHECK(pins.check(b, "000002", "two", "10.0.0.8").result == BoothPin::Result::Wrong);
        CHECK(pins.check(a, "111111", "one", "10.0.0.8").result == BoothPin::Result::Ok);
        const BoothPin::Outcome o = pins.check(b, "000003", "two", "10.0.0.8");
        CHECK(o.result == BoothPin::Result::Wrong);
        CHECK_EQ(o.lockoutSeconds, 30);
        CHECK(pins.check(b, "222222", "three", "10.0.0.8").result == BoothPin::Result::RateLimited);
    }
}

void testIdleBucketsAreForgotten()
{
    SECTION("BoothPin: a bucket left alone for an hour is forgotten");
    BoothPin pins;
    Clock clock;
    pins.setClockForTesting([&clock]() { return clock.now; });
    pins.setGeneratorForTesting([]() { return QStringLiteral("482913"); });
    pins.mint(kStream);
    pins.check(kStream, "000000", "bob", "10.0.0.1");
    pins.check(kStream, "000000", "bob", "10.0.0.1");
    clock.now += 3601;
    const BoothPin::Outcome o = pins.check(kStream, "000000", "bob", "10.0.0.1");
    CHECK_EQ(o.remainingAttempts, 2); // a first failure again, not a third
    CHECK_EQ(o.lockoutSeconds, 0);
}

} // namespace

void run_booth_pin_tests()
{
    testWhatAPinIs();
    testAChangeIsAChange();
    testTheRightPin();
    testGuessingMovesThePin();
    testTheSessionsBucket();
    testTheAddresssBucket();
    testOneStreamsPinBuysNoTriesAtAnother();
    testIdleBucketsAreForgotten();
}
