/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * When a stream worker told to stop may exit (RelayGraphWatch.h): once the
 * relay graph it let go is gone, its engine and its thread included, and no
 * later than a bound counted from the start of its teardown (A125: a worker
 * that always waited 3 s was killed by the server's 5 s when its teardown
 * began late).
 */

#include "../src/streaming/worker/RelayGraphWatch.h"

#include "test_framework.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QSemaphore>
#include <QThread>

#include <atomic>

namespace {

/// Pump this thread's events until @p done or @p ms pass.
template <class F> bool pumpUntil(F done, int ms = 3000)
{
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return true;
}

/// An engine whose destructor holds until released, as a Moonlight shim's
/// does while LiStartConnection has not returned.
struct HeldEngine : QObject
{
    explicit HeldEngine(QSemaphore* release, QObject* parent)
        : QObject(parent)
        , m_Release(release)
    {}
    ~HeldEngine() override { m_Release->acquire(); }
    QSemaphore* m_Release;
};

/// The worker's own way (Session.cpp, spawnRelayThread): the root on a thread
/// of its own, which quits once the root is destroyed and deletes itself.
QThread* spawnLikeASession(QObject* root)
{
    QThread* t = new QThread();
    root->moveToThread(t);
    QObject::connect(root, &QObject::destroyed, t, &QThread::quit);
    QObject::connect(t, &QThread::finished, t, &QThread::deleteLater);
    t->start();
    return t;
}

} // namespace

void run_relay_graph_watch_tests()
{
    SECTION("RelayGraphWatch — nothing watched: gone at once");
    {
        RelayGraphWatch graph;
        CHECK(graph.empty());
        CHECK(graph.gone());

        QObject context;
        QElapsedTimer since;
        since.start();
        int calls = 0;
        bool inTime = false;
        graph.whenGone(&context, since, RelayGraphWatch::kBoundMs,
                       [&](bool t, qint64, const QString&) {
                           ++calls;
                           inTime = t;
                       });
        CHECK(pumpUntil([&]() { return calls > 0; }, 1000));
        CHECK(inTime);
        // Called once, the poll stopped.
        pumpUntil([]() { return false; }, 100);
        CHECK_EQ(calls, 1);
    }

    SECTION("RelayGraphWatch — a root on this thread: gone once deleted, no thread to wait for");
    {
        auto* root = new QObject();
        RelayGraphWatch graph;
        graph.watch(root, nullptr);
        CHECK(!graph.empty());
        CHECK(!graph.gone());
        delete root;
        CHECK(graph.gone());
    }

    SECTION("RelayGraphWatch — the relay gone is not enough: its engine and thread too");
    {
        QSemaphore release;
        auto* relay = new QObject();
        auto* engine = new HeldEngine(&release, relay);
        QPointer<QThread> thread = spawnLikeASession(relay);
        QPointer<QObject> relayGuard = relay;

        RelayGraphWatch graph;
        graph.watch(relay, engine);
        CHECK(!graph.gone());

        relay->deleteLater();
        // Qt clears the relay's guards before it deletes its children: the
        // relay reads as gone while its engine is still being destroyed.
        CHECK(pumpUntil([&]() { return relayGuard.isNull(); }));
        pumpUntil([]() { return false; }, 50);
        CHECK(!graph.gone());
        CHECK(thread && !thread->isFinished());

        release.release();
        CHECK(pumpUntil([&]() { return graph.gone(); }));
        // Gone means the thread finished too: nothing of the graph runs.
        CHECK(!thread || thread->isFinished());
        pumpUntil([&]() { return thread.isNull(); });
        CHECK(thread.isNull());
    }

    SECTION("RelayGraphWatch — done as soon as the graph is down, well before the bound");
    {
        QSemaphore release;
        auto* relay = new QObject();
        auto* engine = new HeldEngine(&release, relay);
        QPointer<QThread> thread = spawnLikeASession(relay);

        RelayGraphWatch graph;
        graph.watch(relay, engine);
        QObject context;
        QElapsedTimer since;
        since.start();
        int calls = 0;
        bool inTime = false;
        qint64 elapsed = -1;
        graph.whenGone(&context, since, RelayGraphWatch::kBoundMs,
                       [&](bool t, qint64 ms, const QString&) {
                           ++calls;
                           inTime = t;
                           elapsed = ms;
                       });
        relay->deleteLater();
        // Still held: not done.
        pumpUntil([]() { return false; }, 100);
        CHECK_EQ(calls, 0);
        release.release();
        CHECK(pumpUntil([&]() { return calls > 0; }));
        CHECK(inTime);
        CHECK(elapsed >= 0 && elapsed < RelayGraphWatch::kBoundMs);
        CHECK(graph.gone());
        pumpUntil([]() { return false; }, 100);
        CHECK_EQ(calls, 1);
        pumpUntil([&]() { return thread.isNull(); });
    }

    SECTION("RelayGraphWatch — a wedged graph: done at the bound, not in time");
    {
        QSemaphore release;
        auto* relay = new QObject();
        auto* engine = new HeldEngine(&release, relay);
        QPointer<QThread> thread = spawnLikeASession(relay);

        QPointer<QObject> relayGuard = relay;

        RelayGraphWatch graph;
        graph.watch(relay, engine);
        QElapsedTimer since;
        since.start();
        relay->deleteLater();
        // The relay's destruction under way, held in its engine's.
        CHECK(pumpUntil([&]() { return relayGuard.isNull(); }));

        QObject context;
        int calls = 0;
        bool inTime = true;
        qint64 elapsed = -1;
        QString left;
        constexpr int kBound = 300;
        graph.whenGone(&context, since, kBound, [&](bool t, qint64 ms, const QString& l) {
            ++calls;
            inTime = t;
            elapsed = ms;
            left = l;
        });
        CHECK(pumpUntil([&]() { return calls > 0; }));
        CHECK(!inTime);
        CHECK(elapsed >= kBound);
        CHECK(!graph.gone());
        // The relay itself reads as gone, its engine and thread do not.
        CHECK_EQ(left, QStringLiteral("1 object, 1 thread"));
        // Let it go, so that no thread outlives the test.
        release.release();
        CHECK(pumpUntil([&]() { return thread.isNull(); }));
        CHECK_EQ(calls, 1);
    }

    SECTION("RelayGraphWatch — the bound counts from the start of the teardown, not the call");
    {
        QSemaphore release;
        auto* relay = new QObject();
        auto* engine = new HeldEngine(&release, relay);
        QPointer<QThread> thread = spawnLikeASession(relay);

        RelayGraphWatch graph;
        graph.watch(relay, engine);
        QElapsedTimer since;
        since.start();
        // The teardown was held up before it got to wait (a main thread busy
        // elsewhere): past the bound already.
        constexpr int kBound = 500;
        QThread::msleep(kBound + 100);

        QObject context;
        int calls = 0;
        bool inTime = true;
        qint64 elapsed = -1;
        QElapsedTimer sinceCall;
        sinceCall.start();
        qint64 waitedAfterCall = -1;
        graph.whenGone(&context, since, kBound, [&](bool t, qint64 ms, const QString&) {
            ++calls;
            inTime = t;
            elapsed = ms;
            waitedAfterCall = sinceCall.elapsed();
        });
        relay->deleteLater();
        CHECK(pumpUntil([&]() { return calls > 0; }));
        CHECK(!inTime);
        CHECK(elapsed >= kBound + 100);
        // Done at its first look, not a whole bound after the call.
        CHECK(waitedAfterCall >= 0 && waitedAfterCall < kBound - 100);
        release.release();
        CHECK(pumpUntil([&]() { return thread.isNull(); }));
    }

    SECTION("RelayGraphWatch — another condition: not gone until it is met, named while it is not");
    {
        bool settled = false;
        RelayGraphWatch graph;
        graph.until("libdatachannel", [&]() { return settled; });
        CHECK(!graph.empty());
        CHECK(!graph.gone());
        CHECK_EQ(graph.left(), QStringLiteral("libdatachannel"));

        QObject context;
        QElapsedTimer since;
        since.start();
        int calls = 0;
        bool inTime = false;
        graph.whenGone(&context, since, RelayGraphWatch::kBoundMs,
                       [&](bool t, qint64, const QString& l) {
                           ++calls;
                           inTime = t && l.isEmpty();
                       });
        pumpUntil([]() { return false; }, 100);
        CHECK_EQ(calls, 0);
        settled = true;
        CHECK(pumpUntil([&]() { return calls > 0; }));
        CHECK(inTime);
        CHECK(graph.gone());
        CHECK(graph.left().isEmpty());
    }

    SECTION("RelayGraphWatch — the graph and the other condition, both waited for");
    {
        QSemaphore release;
        auto* relay = new QObject();
        auto* engine = new HeldEngine(&release, relay);
        QPointer<QThread> thread = spawnLikeASession(relay);
        bool settled = false;

        RelayGraphWatch graph;
        graph.watch(relay, engine);
        graph.until("libdatachannel", [&]() { return settled; });
        relay->deleteLater();
        release.release();
        CHECK(pumpUntil([&]() { return thread.isNull(); }));
        // The graph is down, the condition is not: still not gone.
        CHECK(!graph.gone());
        CHECK_EQ(graph.left(), QStringLiteral("libdatachannel"));
        settled = true;
        CHECK(graph.gone());
    }

    SECTION("RelayGraphWatch — nothing is called once its context is gone");
    {
        RelayGraphWatch graph;
        auto* context = new QObject();
        QElapsedTimer since;
        since.start();
        int calls = 0;
        graph.whenGone(context, since, RelayGraphWatch::kBoundMs,
                       [&](bool, qint64, const QString&) { ++calls; });
        delete context;
        pumpUntil([]() { return false; }, 100);
        CHECK_EQ(calls, 0);
    }
}
