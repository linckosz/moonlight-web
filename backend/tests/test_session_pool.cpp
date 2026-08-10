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
    SessionPool pool(48001, 2, 5);

    // Reserved slots exist up front — they are addressed by index, never acquired.
    CHECK_EQ(pool.size(), 2);
    CHECK_EQ(pool.reservedSlots(), 2);

    // Ports and paths are the frontend-facing contract.
    CHECK_EQ(pool.signalingPort(0), quint16(48001));
    CHECK_EQ(pool.signalingPort(1), quint16(48011));
    CHECK_EQ(pool.signalingPort(2), quint16(48021));
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
    pool.at(b).deviceSessionId = QStringLiteral("device-b");

    const int c = pool.acquire();
    CHECK_EQ(c, 4);
    pool.at(c).clientUniqueId = QStringLiteral("CCCC000000000003");

    // Capped at maxSlots.
    CHECK_EQ(pool.acquire(), -1);

    // Lookups.
    CHECK_EQ(pool.indexOfClientUniqueId(QStringLiteral("AAAA000000000001")), 2);
    CHECK_EQ(pool.indexOfDeviceSession(QStringLiteral("device-b")), 3);
    CHECK_EQ(pool.indexOfClientUniqueId(QStringLiteral("nope")), -1);
    CHECK_EQ(pool.indexOfDeviceSession(QString()), -1); // empty never matches

    // release() frees the index for reuse rather than shrinking the pool.
    pool.release(b);
    CHECK_EQ(pool.size(), 5);
    CHECK_EQ(pool.indexOfDeviceSession(QStringLiteral("device-b")), -1);
    CHECK_EQ(pool.acquire(), 3); // the freed index comes back first
    pool.release(3);

    // A slot claimed but not yet attached to a worker still counts as taken,
    // otherwise two sessions would collide on the same port and /wsN path.
    const int d = pool.acquire();
    CHECK_EQ(d, 3);
    pool.at(d).deviceSessionId = QStringLiteral("device-d");
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

    pool.at(2).worker = nullptr;
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
}
