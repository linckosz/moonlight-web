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
    CHECK_EQ(pool.anyOtherLive(0), true);  // slot 2 is live
    CHECK_EQ(pool.anyOtherLive(2), true);  // slot 0 is live
    CHECK_EQ(pool.anyOtherLive(1), true);  // both others are live

    // Per-host scoping: "sessions share a running app" only holds within one
    // host, so a live slot on another host must not vouch for this one.
    pool.at(0).hostUuid = QStringLiteral("host-A");
    pool.at(2).hostUuid = QStringLiteral("host-B");
    CHECK_EQ(pool.anyOtherLive(0), true);                              // unscoped: sees host-B
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
}
