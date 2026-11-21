/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"

#include "backend/PairingOwners.h"
#include "server/HostWritePacing.h"
#include "server/routes/HostManualAdd.h"
#include "server/routes/HostWrites.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <functional>
#include <utility>

namespace {

struct FakeManager
{
    int scans = 0;
    bool lastScanAdmin = false;
    int restarts = 0;
    int restartStatus = 200;
    int wakes = 0;
    int wakeStatus = 200;
    int adds = 0;
    int addStatus = 200;
    QString addMessage;
    bool lastAddMayMove = false;
    std::function<void()> duringAdd;
    QStringList pairCalls; // "start|uuid|caller|admin" or "pin|uuid|caller"

    void handleScanRequest(bool admin)
    {
        ++scans;
        lastScanAdmin = admin;
    }

    void handleRestartHost(const QString&, ResponseCallback respond)
    {
        ++restarts;
        respond(
            HttpResponse::json({{"status", restartStatus == 200 ? "ok" : "error"}}, restartStatus));
    }

    std::pair<int, QJsonObject> handleWakeHost(const QString& uuid)
    {
        ++wakes;
        if (wakeStatus != 200) return {wakeStatus, {{"status", "error"}}};
        return {200, {{"status", "ok"}, {"message", "Wake-on-LAN packet sent to " + uuid}}};
    }

    std::pair<int, QJsonObject> handleAddManualHost(const QString&, bool mayMoveHost)
    {
        ++adds;
        lastAddMayMove = mayMoveHost;
        if (duringAdd) duringAdd(); // what the nested event loops let in
        if (addStatus != 200) return {addStatus, {{"status", "error"}, {"message", addMessage}}};
        return {200, {{"hosts", QJsonArray{QJsonObject{{"uuid", "uuid-new"}}}}}};
    }

    std::pair<int, QJsonObject> handleStartPairing(const QString& uuid, const QString& caller,
                                                   bool admin)
    {
        pairCalls << QStringLiteral("start|%1|%2|%3")
                         .arg(uuid, caller, QLatin1String(admin ? "admin" : "-"));
        return {200, {{"status", "initiated"}}};
    }

    void handleSubmitPin(const QString& uuid, const QString& caller, ResponseCallback respond)
    {
        pairCalls << QStringLiteral("pin|%1|%2").arg(uuid, caller);
        respond(HttpResponse::json({{"status", "awaiting_pin"}}));
    }
};

struct Rig
{
    qint64 clock = 1000000;
    HostWritePacing pacing{[this]() { return clock; }};
    FakeManager manager;
    RestRouter router;

    Rig()
    {
        const HostWriteCaller caller = [](const HttpRequest& req) {
            return req.headers.value(QStringLiteral("x-test-session"));
        };
        registerHostWrites(router, manager, pacing, caller);
        registerHostManualAdd(router, manager, pacing, caller);
    }

    HttpResponse send(const QString& path, bool admin, const QString& session = "s1",
                      const QByteArray& body = "{}") const
    {
        HttpRequest req;
        req.method = QStringLiteral("POST");
        req.path = path;
        req.body = body;
        req.isLocal = admin;
        req.headers.insert(QStringLiteral("x-test-session"), session);
        HttpResponse reply;
        router.dispatchAsync(req, [&reply](HttpResponse resp) { reply = std::move(resp); });
        return reply;
    }

    HttpResponse add(bool admin, const QString& session = "s1") const
    {
        return send("/api/hosts/manual", admin, session, R"({"address":"192.0.2.40:47989"})");
    }
};

QJsonObject bodyOf(const HttpResponse& r)
{
    return QJsonDocument::fromJson(r.body).object();
}

const QString kRestart = QStringLiteral("/api/hosts/uuid-1/restart");
const QString kWake = QStringLiteral("/api/hosts/uuid-1/wol");
const QString kScan = QStringLiteral("/api/hosts/scan");

void pacedRestarts()
{
    Rig rig;
    CHECK_EQ(rig.send(kRestart, false).statusCode, 200);
    CHECK_EQ(rig.manager.restarts, 1);

    // Again within the minute: refused before anything is stopped, and said so.
    rig.clock += 30000;
    const HttpResponse soon = rig.send(kRestart, false, "s2");
    CHECK_EQ(soon.statusCode, 429);
    CHECK_EQ(bodyOf(soon)["code"].toString(), QStringLiteral("restart_too_soon"));
    CHECK_EQ(soon.headers.value("Retry-After"), QStringLiteral("30"));
    CHECK_EQ(rig.manager.restarts, 1);

    // A refusal does not push the window back: it ends a minute after the restart.
    rig.clock += 29999;
    CHECK_EQ(rig.send(kRestart, false).statusCode, 429);
    CHECK_EQ(rig.send(kRestart, false).headers.value("Retry-After"), QStringLiteral("1"));
    rig.clock += 1;
    CHECK_EQ(rig.send(kRestart, false).statusCode, 200);
    CHECK_EQ(rig.manager.restarts, 2);

    // Another host has its own window.
    CHECK_EQ(rig.send(QStringLiteral("/api/hosts/uuid-2/restart"), false).statusCode, 200);
    CHECK_EQ(rig.manager.restarts, 3);

    // The administrator is not held back, and opens the window for the others.
    rig.clock += 60000;
    CHECK_EQ(rig.send(kRestart, true).statusCode, 200);
    CHECK_EQ(rig.send(kRestart, true).statusCode, 200);
    CHECK_EQ(rig.manager.restarts, 5);
    CHECK_EQ(rig.send(kRestart, false).statusCode, 429);
    CHECK_EQ(rig.manager.restarts, 5);

    // Turned down before anything was done (no such host, no way to restart
    // it): no window.
    Rig none;
    for (int status : {404, 501}) {
        none.manager.restartStatus = status;
        CHECK_EQ(none.send(kRestart, false).statusCode, status);
    }
    none.manager.restartStatus = 200;
    CHECK_EQ(none.send(kRestart, false).statusCode, 200);
    CHECK_EQ(none.manager.restarts, 3);

    // Once something was tried, the window stays, whatever came of it.
    Rig failed;
    failed.manager.restartStatus = 502;
    CHECK_EQ(failed.send(kRestart, false).statusCode, 502);
    failed.manager.restartStatus = 200;
    CHECK_EQ(failed.send(kRestart, false).statusCode, 429);
}

void pacedScans()
{
    Rig rig;
    const HttpResponse first = rig.send(kScan, false);
    const HttpResponse second = rig.send(kScan, false, "s2");
    // Both told the same, one scan made, and never one that extends discovery.
    CHECK_EQ(first.statusCode, 200);
    CHECK_EQ(second.statusCode, 200);
    CHECK_EQ(first.body, second.body);
    CHECK_EQ(bodyOf(second)["status"].toString(), QStringLiteral("scanning"));
    CHECK_EQ(rig.manager.scans, 1);
    CHECK(!rig.manager.lastScanAdmin);

    // The administrator's goes through, as the administrator's.
    CHECK_EQ(rig.send(kScan, true).statusCode, 200);
    CHECK_EQ(rig.manager.scans, 2);
    CHECK(rig.manager.lastScanAdmin);

    rig.clock += 29999;
    rig.send(kScan, false);
    CHECK_EQ(rig.manager.scans, 2);
    rig.clock += 1;
    rig.send(kScan, false);
    CHECK_EQ(rig.manager.scans, 3);
    CHECK(!rig.manager.lastScanAdmin);
}

void pacedWakes()
{
    Rig rig;
    const HttpResponse first = rig.send(kWake, false);
    const HttpResponse again = rig.send(kWake, false, "s2");
    // A second press gets the very same answer, and sends nothing.
    CHECK_EQ(first.statusCode, 200);
    CHECK_EQ(again.statusCode, 200);
    CHECK_EQ(first.body, again.body);
    CHECK_EQ(rig.manager.wakes, 1);
    CHECK_EQ(rig.send(kWake, true).body, first.body);
    CHECK_EQ(rig.manager.wakes, 1);

    // Another host is woken; ten seconds on, this one is again.
    rig.send(QStringLiteral("/api/hosts/uuid-2/wol"), false);
    CHECK_EQ(rig.manager.wakes, 2);
    rig.clock += 10000;
    rig.send(kWake, false);
    CHECK_EQ(rig.manager.wakes, 3);

    // A packet that was not sent is not given again.
    Rig failing;
    failing.manager.wakeStatus = 400;
    CHECK_EQ(failing.send(kWake, false).statusCode, 400);
    CHECK_EQ(failing.send(kWake, false).statusCode, 400);
    CHECK_EQ(failing.manager.wakes, 2);
}

void pacedAdds()
{
    Rig rig;
    CHECK_EQ(rig.add(false).statusCode, 200);
    CHECK_EQ(rig.manager.adds, 1);
    CHECK(!rig.manager.lastAddMayMove);

    // The same session again within ten seconds: refused before the body is
    // read, whatever it holds.
    rig.clock += 5000;
    const HttpResponse again = rig.send("/api/hosts/manual", false, "s1", "not json");
    CHECK_EQ(again.statusCode, 429);
    CHECK_EQ(bodyOf(again)["code"].toString(), QStringLiteral("too_many_attempts"));
    CHECK_EQ(again.headers.value("Retry-After"), QStringLiteral("5"));
    CHECK_EQ(rig.manager.adds, 1);

    // Another session may; the administrator is not counted.
    CHECK_EQ(rig.add(false, "s2").statusCode, 200);
    CHECK_EQ(rig.add(true).statusCode, 200);
    CHECK(rig.manager.lastAddMayMove);
    CHECK_EQ(rig.manager.adds, 3);

    // An attempt counts whatever comes of it, a body without an address too.
    rig.clock += 10000;
    CHECK_EQ(rig.send("/api/hosts/manual", false, "s3", "{}").statusCode, 400);
    CHECK_EQ(rig.add(false, "s3").statusCode, 429);

    // Thirty an hour for the whole machine, however many sessions ask.
    Rig busy;
    for (int i = 0; i < HostWritePacing::kAddsPerWindow; ++i) {
        CHECK_EQ(busy.add(false, QStringLiteral("s%1").arg(i)).statusCode, 200);
        busy.clock += 60000;
    }
    CHECK_EQ(busy.add(false, "fresh").statusCode, 429);
    CHECK_EQ(busy.manager.adds, HostWritePacing::kAddsPerWindow);
    CHECK_EQ(busy.add(true).statusCode, 200);
    // The first of them leaves the hour: one more.
    busy.clock = 1000000 + HostWritePacing::kAddWindowMs;
    CHECK_EQ(busy.add(false, "fresh").statusCode, 200);
}

void oneAddAtATime()
{
    // The manager resolves and asks in nested event loops: another request can
    // come in meanwhile.
    Rig rig;
    HttpResponse during;
    HttpResponse duringAdmin;
    rig.manager.duringAdd = [&]() {
        rig.manager.duringAdd = nullptr;
        during = rig.add(false, "s2");
        duringAdmin = rig.add(true);
    };
    CHECK_EQ(rig.add(false).statusCode, 200);
    CHECK_EQ(during.statusCode, 429);
    CHECK_EQ(bodyOf(during)["code"].toString(), QStringLiteral("too_many_attempts"));
    CHECK_EQ(duringAdmin.statusCode, 200);
    CHECK_EQ(rig.manager.adds, 2);

    // Answered, the next one goes ahead.
    CHECK_EQ(rig.add(false, "s2").statusCode, 200);
}

void oneFailureForEveryone()
{
    // Every way an add fails reads the same to anyone but the administrator:
    // shut, filtered, open on something else, or a host that will not move.
    QByteArray first;
    const QList<std::pair<int, QString>> failures = {
        {400, QStringLiteral("Connection refused")},
        {400, QStringLiteral("Operation canceled")},
        {400, QStringLiteral("Malformed XML")},
        {400, QStringLiteral("DNS resolution failed")},
        {409, QStringLiteral("This address belongs to another host")},
        {502, QStringLiteral("Bad gateway")}};
    for (const auto& [status, message] : failures) {
        Rig rig;
        rig.manager.addStatus = status;
        rig.manager.addMessage = message;
        const HttpResponse asSession = rig.add(false);
        CHECK_EQ(asSession.statusCode, 400);
        CHECK_EQ(bodyOf(asSession)["code"].toString(), QStringLiteral("host_add_failed"));
        CHECK(!asSession.body.contains(message.toUtf8()));
        if (first.isEmpty()) first = asSession.body;
        CHECK_EQ(asSession.body, first);

        // The administrator keeps the detail.
        const HttpResponse asAdmin = rig.add(true);
        CHECK_EQ(asAdmin.statusCode, status);
        CHECK_EQ(bodyOf(asAdmin)["message"].toString(), message);
        CHECK(!bodyOf(asAdmin).contains("code"));
    }
}

void pairingRoutesNameTheCaller()
{
    Rig rig;
    rig.send(QStringLiteral("/api/hosts/uuid-1/pair/start"), false, "s1");
    rig.send(QStringLiteral("/api/hosts/uuid-1/pair/start"), true, "s2");
    rig.send(QStringLiteral("/api/hosts/uuid-1/pair"), false, "s1");
    CHECK_EQ(rig.manager.pairCalls.join(QLatin1Char(',')),
             QStringLiteral("start|uuid-1|s1|-,start|uuid-1|s2|admin,pin|uuid-1|s1"));
}

void pairingOwners()
{
    using Start = PairingOwners::Start;
    const QString host = QStringLiteral("uuid-1");
    const QString mine = QStringLiteral("aaaa");
    const QString theirs = QStringLiteral("bbbb");
    QStringList gone;
    const PairingOwners::SessionAlive alive = [&gone](const QString& id) {
        return !gone.contains(id);
    };
    const qint64 t0 = 5000000;

    PairingOwners owners;
    // Nothing held: anyone starts.
    CHECK(owners.decideStart(host, false, false, theirs, false, t0, alive) == Start::Fresh);
    owners.begin(host, mine, t0);

    // Its owner: the same PIN while its chain runs, a new one otherwise.
    CHECK(owners.decideStart(host, true, true, mine, false, t0, alive) == Start::ResumeOwn);
    CHECK(owners.decideStart(host, true, false, mine, false, t0, alive) == Start::Replace);

    // Anyone else: refused, chain or no chain; neither submits.
    CHECK(owners.decideStart(host, true, false, theirs, false, t0, alive) == Start::Busy);
    CHECK(owners.decideStart(host, true, true, theirs, false, t0, alive) == Start::Busy);
    CHECK(!owners.maySubmit(host, theirs));
    CHECK(owners.maySubmit(host, mine));
    CHECK(!owners.maySubmit(host, QString()));
    CHECK(owners.decideStart(host, true, false, QString(), false, t0, alive) == Start::Busy);

    // The administrator replaces it, but never under a running chain.
    CHECK(owners.decideStart(host, true, false, theirs, true, t0, alive) == Start::Replace);
    CHECK(owners.decideStart(host, true, true, theirs, true, t0, alive) == Start::Busy);

    // Driven by its owner, it does not expire.
    const qint64 later = t0 + PairingOwners::kIdleMs - 1;
    owners.touch(host, later);
    CHECK(!owners.expired(host, later + PairingOwners::kIdleMs - 1, alive));
    CHECK(owners.decideStart(host, true, false, theirs, false, later + 1000, alive) == Start::Busy);

    // Left alone five minutes: another may start — but while its chain still
    // runs, nothing is freed and no PIN is given (Busy).
    const qint64 idle = later + PairingOwners::kIdleMs;
    CHECK(owners.expired(host, idle, alive));
    CHECK(owners.decideStart(host, true, false, theirs, false, idle, alive) == Start::Replace);
    CHECK(owners.decideStart(host, true, true, theirs, false, idle, alive) == Start::Busy);
    CHECK_EQ(owners.expiredHosts(idle, alive), QStringList{host});
    CHECK(owners.expiredHosts(idle - 1, alive).isEmpty());

    // Its session gone: the same, at once.
    gone << mine;
    CHECK(owners.expired(host, later, alive));
    CHECK(owners.decideStart(host, true, false, theirs, false, later, alive) == Start::Replace);
    CHECK(owners.decideStart(host, true, true, theirs, false, later, alive) == Start::Busy);

    // A caller that is not a session is never "gone".
    PairingOwners local;
    local.begin(host, QStringLiteral("local:provisioning"), t0);
    gone << QStringLiteral("local:provisioning");
    CHECK(!local.expired(host, t0, alive));
    CHECK(local.isOwner(host, QStringLiteral("local:provisioning")));

    // Cleared, it is nobody's.
    owners.clear(host);
    CHECK(!owners.has(host));
    CHECK(owners.maySubmit(host, theirs));
    CHECK(owners.expired(host, t0, alive));
}

} // namespace

void run_host_writes_tests()
{
    SECTION("Host writes: paced, pairing bound to its session");
    pacedRestarts();
    pacedScans();
    pacedWakes();
    pacedAdds();
    oneAddAtATime();
    oneFailureForEveryone();
    pairingRoutesNameTheCaller();
    pairingOwners();
}
