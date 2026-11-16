/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The router's holes as the process quits: every hole taken off within a
 * deadline, the streams' shared ports first; nothing new asked once the quit
 * has begun; what the router does not take off left to the record; and the
 * wait on the router's thread that ends at its budget while the thread goes
 * on, with what it touches outliving it.
 */
#include "test_framework.h"
#include "network/BoundedRun.h"
#include "network/RouterPortCore.h"
#include "network/RouterPortPools.h"

#include <QElapsedTimer>
#include <QMap>
#include <QPair>
#include <QThread>

#include <atomic>
#include <cstdio>
#include <thread>
#include <condition_variable>
#include <mutex>

namespace {

using Purpose = RouterPortCore::Purpose;

/// A router as a table, whose removals can be refused, or held until the
/// test lets them go.
struct QuitGateway : IUpnpGateway
{
    struct Row
    {
        QString client;
        uint16_t internal = 0;
    };

    QString lan = QStringLiteral("192.168.1.20");
    QMap<QPair<uint16_t, QString>, Row> table;
    int adds = 0;
    QList<QPair<uint16_t, QString>> removed;
    bool refuseRemovals = false;

    // Held removals: the first removal waits until the test lets it go.
    bool holdFirstRemoval = false;
    std::mutex lock;
    std::condition_variable cv;
    bool letGo = false;
    std::atomic<bool> holding{false};

    QString lanAddress() const override { return lan; }

    bool add(uint16_t ext, uint16_t in, uint32_t, const QString&, const QString& proto) override
    {
        ++adds;
        table[qMakePair(ext, proto)] = {lan, in};
        return true;
    }

    Lookup entry(uint16_t ext, const QString& proto, Entry& out) override
    {
        const auto key = qMakePair(ext, proto);
        if (!table.contains(key)) return Lookup::None;
        out.client = table[key].client;
        out.internalPort = table[key].internal;
        out.leaseLeft = 0;
        return Lookup::Found;
    }

    bool remove(uint16_t ext, const QString& proto) override
    {
        if (holdFirstRemoval) {
            holdFirstRemoval = false;
            holding = true;
            std::unique_lock<std::mutex> l(lock);
            cv.wait(l, [this]() { return letGo; });
            holding = false;
        }
        removed.append(qMakePair(ext, proto));
        if (refuseRemovals) return false;
        return table.remove(qMakePair(ext, proto)) > 0;
    }

    void release()
    {
        {
            std::lock_guard<std::mutex> l(lock);
            letGo = true;
        }
        cv.notify_all();
    }
};

const auto kFree = [](uint16_t) { return true; };

RouterPortCore::Request tunnel()
{
    RouterPortCore::Request r;
    r.purpose = Purpose::Tunnel;
    for (const uint16_t p : mw::routerports::kTunnelPreferred)
        r.preferred.append(p);
    r.poolBegin = mw::routerports::kTunnelPoolBegin;
    r.poolEnd = mw::routerports::kTunnelPoolEnd;
    return r;
}

RouterPortCore::Request media(int slot)
{
    RouterPortCore::Request r;
    r.purpose = Purpose::Media;
    r.slot = slot;
    r.internalPort = static_cast<uint16_t>(mw::routerports::kMediaBasePort + slot);
    r.preferred.append(r.internalPort);
    r.poolBegin = mw::routerports::kMediaPoolBegin;
    r.poolEnd = mw::routerports::kMediaPoolEnd;
    return r;
}

/// A worker thread's object, as the allocator's.
class Worker : public QObject
{};

} // namespace

void run_router_quit_tests()
{
    SECTION("router quit: every hole is taken off, the tunnel's before the slots'");
    {
        QuitGateway gw;
        RouterPortCore core(gw, kFree);
        const auto m0 = core.claim(media(0));
        const auto t = core.claim(tunnel());
        const auto m1 = core.claim(media(1));
        CHECK(m0.ok && t.ok && m1.ok);
        const auto out = core.releaseBeforeExit({});
        CHECK(core.closed());
        CHECK(out.released == 3);
        CHECK(out.left.isEmpty());
        CHECK(core.held().isEmpty());
        CHECK(gw.table.isEmpty());
        // The tunnel's entries go first, whatever order they were claimed in.
        CHECK(!gw.removed.isEmpty() && gw.removed.first().first == t.claim.external);
    }

    SECTION("router quit: once it has begun, nothing new is written to the router");
    {
        QuitGateway gw;
        RouterPortCore core(gw, kFree);
        const auto t = core.claim(tunnel());
        CHECK(t.ok);
        core.releaseBeforeExit({});
        const int adds = gw.adds;
        const auto again = core.claim(tunnel());
        CHECK(!again.ok);
        CHECK(core.renew().isEmpty());
        CHECK(core.renew(t.claim.external).isEmpty());
        CHECK(gw.adds == adds);
        CHECK(gw.table.isEmpty());
    }

    SECTION("router quit: close() alone shuts the door on claims and renewals");
    {
        QuitGateway gw;
        RouterPortCore core(gw, kFree);
        const auto t = core.claim(tunnel());
        CHECK(t.ok);
        const int adds = gw.adds;
        core.close();
        CHECK(!core.claim(media(0)).ok);
        core.renew();
        core.renew(t.claim.external);
        CHECK(gw.adds == adds);
    }

    SECTION("router quit: no call begun after the deadline; the rest is left");
    {
        QuitGateway gw;
        RouterPortCore core(gw, kFree);
        CHECK(core.claim(tunnel()).ok);
        CHECK(core.claim(media(0)).ok);
        CHECK(core.claim(media(1)).ok);
        const int before = gw.table.size();
        int asked = 0;
        // Expires once the first claim's entries have been looked at and
        // their removals asked for: before each of those four calls.
        const auto out = core.releaseBeforeExit([&asked]() { return ++asked > 4; });
        CHECK(out.released == 1);
        CHECK(out.left.size() == 2);
        CHECK(core.held().isEmpty());
        CHECK(gw.table.size() == before - 2);
        for (const auto& c : out.left)
            CHECK(c.purpose == Purpose::Media);
    }

    SECTION("router quit: a removal the router refuses is left, not counted as taken off");
    {
        QuitGateway gw;
        RouterPortCore core(gw, kFree);
        CHECK(core.claim(tunnel()).ok);
        gw.refuseRemovals = true;
        const auto out = core.releaseBeforeExit({});
        CHECK(out.released == 0);
        CHECK(out.left.size() == 1);
    }

    SECTION("router quit: a neighbour's TCP entry on the number is not ours, nor left");
    {
        QuitGateway gw;
        RouterPortCore core(gw, kFree);
        const auto t = core.claim(tunnel());
        CHECK(t.ok && t.claim.tcp);
        gw.table[qMakePair(t.claim.external, QStringLiteral("TCP"))].client =
            QStringLiteral("192.168.1.99");
        const auto out = core.releaseBeforeExit({});
        CHECK(out.released == 1);
        CHECK(out.left.isEmpty());
        CHECK(gw.table.contains(qMakePair(t.claim.external, QStringLiteral("TCP"))));
    }

    SECTION("router quit: the wait ends at its budget, the thread goes on, and is joined");
    {
        QuitGateway gw;
        RouterPortCore core(gw, kFree);
        CHECK(core.claim(tunnel()).ok);
        CHECK(core.claim(media(0)).ok);
        gw.holdFirstRemoval = true;

        // The thread the router's calls are made on: a plain one here, so
        // that every hand-over between the two is one the sanitizer sees.
        std::thread thread;
        const auto post = [&thread](std::function<void()> job) {
            thread = std::thread(std::move(job));
        };
        // What the job writes outlives the thread: it is the test's.
        RouterPortCore::Released out;
        std::atomic<bool> ended{false};
        std::atomic<bool> budgetSpent{false};
        QElapsedTimer clock;
        clock.start();
        const bool inTime = mw::runWithin(
            post,
            [&]() {
                out = core.releaseBeforeExit([&budgetSpent]() { return budgetSpent.load(); });
                ended = true;
            },
            150);
        budgetSpent = true;
        CHECK(!inTime);
        CHECK(clock.elapsed() < 2000);
        CHECK(!ended.load());
        // The thread is in the router's call: let it go, then end the thread
        // before anything it touches goes, as ~RouterPortAllocator does.
        for (int i = 0; i < 200 && !gw.holding.load(); ++i)
            QThread::msleep(5);
        CHECK(gw.holding.load());
        gw.release();
        thread.join();
        CHECK(ended.load());
        // The held call was the tunnel's UDP removal; the budget was spent
        // by the time it came back, so nothing more was begun.
        CHECK(out.released == 0);
        CHECK(out.left.size() == 2);
        CHECK(gw.removed.size() == 1);
    }

    SECTION("router quit: a job that ends in time is said to");
    {
        std::thread thread;
        std::atomic<int> ran{0};
        CHECK(mw::runWithin(
            [&thread](std::function<void()> job) { thread = std::thread(std::move(job)); },
            [&ran]() { ran = 1; }, 2000));
        CHECK(ran.load() == 1);
        thread.join();
    }

#if !defined(__SANITIZE_THREAD__)
    // Through Qt's own queue, as the allocator hands its job over. Not under
    // ThreadSanitizer: Qt is not built for it, and every queued call reads
    // as a race there.
    SECTION("router quit: the job runs on the thread its object lives on");
    {
        QThread thread;
        Worker* worker = new Worker;
        worker->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.start();
        std::atomic<bool> there{false};
        CHECK(mw::runWithin(
            worker, [&there, &thread]() { there = QThread::currentThread() == &thread; }, 2000));
        CHECK(there.load());
        thread.quit();
        thread.wait();
    }
#else
    std::puts("  [SKIPPED under ThreadSanitizer] router quit: the job through Qt's queue");
#endif
}
