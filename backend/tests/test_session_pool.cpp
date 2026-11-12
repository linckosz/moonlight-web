/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "server/SessionPool.h"

#include <QObject>

void run_session_pool_tests()
{
    SECTION("SessionPool");

    // Historical layout: 2 owner slots (primary + standby), 3 player slots.
    SessionPool pool(48001, 48024, 2, 5);

    // Reserved slots exist up front — they are addressed by index, never acquired.
    CHECK_EQ(pool.size(), 2);
    CHECK_EQ(pool.reservedSlots(), 2);

    // Slot 0 keeps its historical port; the rest live two apart in their own
    // block, which the caller placed clear of the media range (48010 + slot) so
    // the two can never land on the same number.
    CHECK_EQ(pool.signalingPort(0), quint16(48001));
    CHECK_EQ(pool.signalingPort(1), quint16(48024));
    CHECK_EQ(pool.signalingPort(2), quint16(48026));
    CHECK_EQ(pool.signalingPort(4), quint16(48030));
    // The relay takes the port above, so consecutive slots must not overlap.
    CHECK(pool.signalingPort(2) > pool.signalingPort(1) + 1);
    CHECK(SessionPool::wsPath(0) == QStringLiteral("/ws"));
    CHECK(SessionPool::wsPath(1) == QStringLiteral("/ws1"));
    CHECK(SessionPool::wsPath(4) == QStringLiteral("/ws4"));

    // Nothing is live on a fresh pool.
    CHECK_EQ(pool.liveCount(), 0);
    CHECK_EQ(pool.live(0), false);
    CHECK_EQ(pool.anyOtherLive(0), false);

    // acquire() starts above the reserved slots and grows the pool.
    const int a = pool.acquire();
    CHECK_EQ(a, 2);
    CHECK_EQ(pool.size(), 3);
    pool.at(a).clientUniqueId = QStringLiteral("AAAA000000000001");

    const int b = pool.acquire();
    CHECK_EQ(b, 3);
    pool.at(b).sessionToken = QStringLiteral("device-b");

    const int c = pool.acquire();
    CHECK_EQ(c, 4);
    pool.at(c).clientUniqueId = QStringLiteral("CCCC000000000003");

    // Capped at maxSlots.
    CHECK_EQ(pool.acquire(), -1);

    // Lookups.
    CHECK_EQ(pool.indexOfClientUniqueId(QStringLiteral("AAAA000000000001")), 2);
    CHECK_EQ(pool.indexOfSessionToken(QStringLiteral("device-b")), 3);
    CHECK_EQ(pool.indexOfClientUniqueId(QStringLiteral("nope")), -1);
    CHECK_EQ(pool.indexOfSessionToken(QString()), -1); // empty never matches

    // release() frees the index for reuse rather than shrinking the pool.
    pool.release(b);
    CHECK_EQ(pool.size(), 5);
    CHECK_EQ(pool.indexOfSessionToken(QStringLiteral("device-b")), -1);
    CHECK_EQ(pool.acquire(), 3); // the freed index comes back first
    pool.release(3);

    // A slot claimed but not yet attached to a worker still counts as taken,
    // otherwise two sessions would collide on the same port and /wsN path.
    const int d = pool.acquire();
    CHECK_EQ(d, 3);
    pool.at(d).sessionToken = QStringLiteral("device-d");
    CHECK_EQ(pool.acquire(), -1); // full again, even with no workers attached

    // Liveness follows the worker pointer.
    QObject worker0;
    QObject worker2;
    pool.at(0).worker = &worker0;
    pool.at(2).worker = &worker2;
    CHECK_EQ(pool.live(0), true);
    CHECK_EQ(pool.live(1), false);
    CHECK_EQ(pool.liveCount(), 2);

    // anyOtherLive drives the quit invariant: a /cancel is only safe when no
    // other slot is still streaming the same host app.
    CHECK_EQ(pool.anyOtherLive(0), true); // slot 2 is live
    CHECK_EQ(pool.anyOtherLive(2), true); // slot 0 is live
    CHECK_EQ(pool.anyOtherLive(1), true); // both others are live

    // Per-host scoping: "sessions share a running app" only holds within one
    // host, so a live slot on another host must not vouch for this one.
    pool.at(0).hostUuid = QStringLiteral("host-A");
    pool.at(2).hostUuid = QStringLiteral("host-B");
    CHECK_EQ(pool.anyOtherLive(0), true);                                  // unscoped: sees host-B
    CHECK_EQ(pool.anyOtherLiveOnHost(0, QStringLiteral("host-A")), false); // scoped: nothing on A
    CHECK_EQ(pool.anyOtherLiveOnHost(2, QStringLiteral("host-B")), false);
    pool.at(1).worker = &worker0;
    pool.at(1).hostUuid = QStringLiteral("host-A");
    CHECK_EQ(pool.anyOtherLiveOnHost(0, QStringLiteral("host-A")), true); // slot 1 shares host A
    pool.at(1).worker = nullptr;
    pool.at(1).hostUuid.clear();

    // A slot still setting up (no host yet) counts as same-host: the safe
    // answer, so no /cancel slips through underneath it.
    pool.at(3).worker = &worker0;
    CHECK_EQ(pool.anyOtherLiveOnHost(0, QStringLiteral("host-A")), true);
    pool.at(3).worker = nullptr;

    // What stop-session takes down for one host: that host's slots, both legs
    // of its dual stream included, and never a stream of another host (a
    // native stream that outlives the Sunshine session closed on its PC).
    pool.at(1).worker = &worker0;
    pool.at(1).hostUuid = QStringLiteral("host-A");
    CHECK(pool.slotsOnHost(QStringLiteral("host-A"), 5) == (QVector<int>{0, 1}));
    CHECK(pool.slotsOnHost(QStringLiteral("host-B"), 5) == (QVector<int>{2}));
    CHECK(pool.slotsOnHost(QStringLiteral("host-C"), 5).isEmpty());
    // Only among the slots asked about: the owner's, not the players' above.
    CHECK(pool.slotsOnHost(QStringLiteral("host-B"), 2).isEmpty());
    // A live slot still setting up is taken as this host's, an idle one is not.
    pool.at(3).worker = &worker0;
    CHECK(pool.slotsOnHost(QStringLiteral("host-B"), 5) == (QVector<int>{2, 3}));
    pool.at(3).worker = nullptr;
    CHECK(pool.slotsOnHost(QStringLiteral("host-B"), 5) == (QVector<int>{2}));
    pool.at(1).worker = nullptr;
    pool.at(1).hostUuid.clear();

    pool.at(2).worker = nullptr;
    pool.at(0).hostUuid.clear();
    pool.at(2).hostUuid.clear();
    CHECK_EQ(pool.anyOtherLive(0), false); // slot 0 is now the only live one
    CHECK_EQ(pool.liveCount(), 1);

    // A QPointer clears itself when the worker dies, so a crashed child does
    // not leave the slot looking live.
    {
        QObject transient;
        pool.at(1).worker = &transient;
        CHECK_EQ(pool.live(1), true);
    }
    CHECK_EQ(pool.live(1), false);

    // Out-of-range access is inert rather than fatal.
    CHECK_EQ(pool.isValid(-1), false);
    CHECK_EQ(pool.isValid(99), false);
    CHECK_EQ(pool.live(99), false);
    pool.release(99); // must not crash

    // ── Slots are opened one at a time, and announced ─────────────────────
    SECTION("SessionPool slot creation");

    QVector<int> opened;
    SessionPool grown(48001, 48024, 2, 4);
    grown.setOnSlotCreated([&opened](int slot) { opened.append(slot); });

    // Nothing is created ahead of need: the reserved slots exist, no more.
    CHECK_EQ(grown.size(), 2);
    CHECK_EQ(opened.size(), 0);

    // Each acquire() creates exactly one slot and announces it before handing
    // it back, so the caller has registered its route by the time the index is
    // in anyone's hands.
    CHECK_EQ(grown.acquire(), 2);
    CHECK_EQ(grown.size(), 3);
    CHECK_EQ(opened.size(), 1);
    CHECK_EQ(opened.value(0), 2);
    grown.at(2).clientUniqueId = QStringLiteral("device-0");

    CHECK_EQ(grown.acquire(), 3);
    CHECK_EQ(opened.size(), 2);
    CHECK_EQ(opened.value(1), 3);
    grown.at(3).clientUniqueId = QStringLiteral("device-1");

    // maxSlots is the wall, and a refusal announces nothing.
    CHECK_EQ(grown.acquire(), -1);
    CHECK_EQ(grown.size(), 4);
    CHECK_EQ(opened.size(), 2);

    // A released index is reused rather than announced again: /wsN indices stay
    // as few as the pool can manage.
    grown.release(2);
    CHECK_EQ(grown.acquire(), 2);
    CHECK_EQ(grown.size(), 4);
    CHECK_EQ(opened.size(), 2);

    // ── Whose stream a request is about ─────────────────────────────────────
    SECTION("SessionPool: an owner's request is about its own stream");
    {
        // Two owner slots, two devices of one owner, two machines.
        SessionPool two(48001, 48024, 2, 5);
        QObject w0, w1;
        two.at(0).worker = &w0;
        two.at(0).hostUuid = QStringLiteral("host-A");
        two.at(0).appId = 1;
        two.at(0).sessionToken = QStringLiteral("device-x");
        two.at(0).clientUniqueId = QStringLiteral("UID-X");
        two.at(1).worker = &w1;
        two.at(1).hostUuid = QStringLiteral("host-B");
        two.at(1).appId = 2;
        two.at(1).sessionToken = QStringLiteral("device-y");
        two.at(1).clientUniqueId = QStringLiteral("UID-Y");
        const QString x = QStringLiteral("device-x");
        const QString y = QStringLiteral("device-y");

        // Each device's board invites to the machine that device streams,
        // whether or not it names the host, and whichever slot it sits in.
        CHECK_EQ(two.ownerSlotFor(y, QString(), 2), 1);
        CHECK_EQ(two.ownerSlotFor(y, QStringLiteral("host-B"), 2), 1);
        CHECK_EQ(two.ownerSlotFor(x, QString(), 2), 0);
        CHECK_EQ(two.ownerSlotFor(x, QStringLiteral("host-A"), 2), 0);
        // A board opened on a host nobody streams finds nothing: it goes cold
        // on the host it names.
        CHECK_EQ(two.ownerSlotFor(y, QStringLiteral("host-C"), 2), -1);
        // A device with no stream of its own, on a host another one streams:
        // that stream, which is what a guest would join there anyway.
        CHECK_EQ(two.ownerSlotFor(QStringLiteral("device-z"), QStringLiteral("host-B"), 2), 1);
        CHECK_EQ(two.ownerSlotFor(QStringLiteral("device-z"), QString(), 2), 0);
        // Localhost has no session row: unknown keeps the first live slot.
        CHECK_EQ(two.ownerSlotFor(QString(), QString(), 2), 0);
        // Only the owner slots count.
        CHECK_EQ(two.ownerSlotFor(y, QStringLiteral("host-B"), 1), -1);
        // A slot with no host is still setting up, and never answers.
        two.at(1).hostUuid.clear();
        CHECK_EQ(two.ownerSlotFor(y, QString(), 2), 0);
        two.at(1).hostUuid = QStringLiteral("host-B");

        // A stop from the device that holds the host is its own; one from a
        // device that was taken over, or from another device, is not.
        CHECK(!two.hostHeldByAnother(QStringLiteral("host-B"), y, QStringLiteral("UID-Y"), 2));
        CHECK(two.hostHeldByAnother(QStringLiteral("host-B"), x, QStringLiteral("UID-X"), 2));
        CHECK(two.hostHeldByAnother(QStringLiteral("host-B"), y, QStringLiteral("UID-OTHER"), 2));
        // Unknown on either side counts as the same device, as everywhere.
        CHECK(!two.hostHeldByAnother(QStringLiteral("host-B"), QString(), QString(), 2));
        CHECK(!two.hostHeldByAnother(QStringLiteral("host-B"), y, QString(), 2));
        // Nobody streams host C, so nobody else holds it.
        CHECK(!two.hostHeldByAnother(QStringLiteral("host-C"), x, QStringLiteral("UID-X"), 2));
        // A slot whose worker is gone holds nothing.
        two.at(1).worker = nullptr;
        CHECK(!two.hostHeldByAnother(QStringLiteral("host-B"), x, QStringLiteral("UID-X"), 2));
    }

    SECTION("SessionPool: the streams of one session, and no one else's");
    {
        // Two owner slots of two devices, an invited player, and an app slot
        // of the first device (the virtual display beside Display 1).
        SessionPool pool(48001, 48024, 2, 6);
        QObject w0, w1, wPlayer, wApp;
        pool.at(0).worker = &w0;
        pool.at(0).sessionToken = QStringLiteral("device-x");
        pool.at(1).worker = &w1;
        pool.at(1).sessionToken = QStringLiteral("device-y");
        const int player = pool.acquire();
        pool.at(player).worker = &wPlayer; // a player has no session token
        const int app = pool.acquire();
        pool.at(app).worker = &wApp;
        pool.at(app).sessionToken = QStringLiteral("device-x");
        const auto is = [](const char* token) {
            return [token](const QString& t) { return t == QLatin1String(token); };
        };

        CHECK(pool.slotsOfSession(is("device-x")) == (QVector<int>{0, app}));
        CHECK(pool.slotsOfSession(is("device-y")) == QVector<int>{1});
        CHECK(pool.slotsOfSession(is("device-z")).isEmpty());
        // A slot without a token is nobody's, even for a test that takes all.
        CHECK(pool.slotsOfSession([](const QString&) { return true; }) ==
              (QVector<int>{0, 1, app}));
        CHECK(pool.slotsOfSession({}).isEmpty());
        // A slot whose worker is gone streams nothing.
        pool.at(app).worker = nullptr;
        CHECK(pool.slotsOfSession(is("device-x")) == QVector<int>{0});
    }

    SECTION("SessionPool: a Stop reaches the app slot of the device that sent it");
    {
        // UM790Pro, 12/11/2026: two devices on one Wolf. The first holds slot
        // 0, the second lands in an app slot, launched with its base uniqueid,
        // and its page stops with the id it derives for that slot — another
        // one. The Stop must still find the slot, or the stream ends on its
        // own and the Wolf lobby it leaves empty keeps running.
        SessionPool pool(48001, 48024, 2, 6);
        QObject wOwner, wSecond, wPlayer;
        const QString host = QStringLiteral("wolf");
        pool.at(0).worker = &wOwner;
        pool.at(0).hostUuid = host;
        pool.at(0).clientUniqueId = QStringLiteral("UID-A");
        pool.at(0).sessionToken = QStringLiteral("device-a");
        const int second = pool.acquire();
        CHECK(second >= 2);
        pool.at(second).worker = &wSecond;
        pool.at(second).hostUuid = host;
        pool.at(second).clientUniqueId = QStringLiteral("UID-B");
        pool.at(second).sessionToken = QStringLiteral("device-b");
        const int player = pool.acquire();
        pool.at(player).worker = &wPlayer; // an invited player has no session token
        pool.at(player).hostUuid = host;
        pool.at(player).clientUniqueId = QStringLiteral("UID-P");
        const QString b = QStringLiteral("device-b");
        const QString derived = QStringLiteral("UID-B-DERIVED");

        // The page names its slot, from its own session: found, whatever id it sends.
        CHECK(pool.quitReachesAppSlot(second, host, derived, b, second));
        CHECK(pool.quitReachesAppSlot(second, host, QString(), b, second));
        // The id the launch carried is enough on its own, as it always was.
        CHECK(pool.quitReachesAppSlot(second, host, QStringLiteral("UID-B"), b, -1));
        CHECK(pool.quitReachesAppSlot(second, host, QStringLiteral("UID-B"), QString(), -1));
        // A derived id with no slot named proves nothing.
        CHECK(!pool.quitReachesAppSlot(second, host, derived, b, -1));
        // Another session never stops it, even naming the slot and its id.
        CHECK(!pool.quitReachesAppSlot(second, host, QStringLiteral("UID-B"),
                                       QStringLiteral("device-a"), second));
        CHECK(!pool.quitReachesAppSlot(second, host, derived, QStringLiteral("device-a"), second));
        // No session at all (localhost) needs the id: naming the slot is not enough.
        CHECK(!pool.quitReachesAppSlot(second, host, derived, QString(), second));
        CHECK(!pool.quitReachesAppSlot(second, host, QString(), QString(), second));
        // Another slot named, another host, a slot with no worker: not this one.
        CHECK(!pool.quitReachesAppSlot(second, host, derived, b, 0));
        CHECK(!pool.quitReachesAppSlot(second, QStringLiteral("sunshine"), derived, b, second));
        CHECK(!pool.quitReachesAppSlot(-1, host, derived, b, -1));
        CHECK(!pool.quitReachesAppSlot(pool.size(), host, derived, b, pool.size()));
        // A player's slot has no token: only its own id reaches it, never a session.
        CHECK(!pool.quitReachesAppSlot(player, host, derived, b, player));
        CHECK(pool.quitReachesAppSlot(player, host, QStringLiteral("UID-P"), QString(), player));
        pool.at(second).worker = nullptr;
        CHECK(!pool.quitReachesAppSlot(second, host, derived, b, second));
    }
}
