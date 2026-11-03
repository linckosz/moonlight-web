/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"

#include "backend/ManualHostMerge.h"
#include "server/RequestGuard.h"
#include "server/routes/HostBackendWrites.h"
#include "server/routes/HostManualAdd.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QStringList>

#include <utility>

namespace {

struct FakeManager
{
    QStringList setCall;
    QString cleared;
    QString provisioned; // uuid|accountName
    QString tornDown;    // uuid|seatId
    QString released;    // uuid|seatId

    void handleSetBackend(const QString& uuid, const QString& type, const QString& apiUrl,
                          const QString& apiToken, const QString& pairUser,
                          const QString& pairPassword, ResponseCallback respond)
    {
        setCall = {uuid, type, apiUrl, apiToken, pairUser, pairPassword};
        respond(HttpResponse::json(QJsonObject{{"status", "paired"}}, 200));
    }

    std::pair<int, QJsonObject> handleClearBackend(const QString& uuid)
    {
        cleared = uuid;
        return {200, QJsonObject{{"status", "ok"}}};
    }

    void handleProvisionSeat(const QString& uuid, const QJsonObject& params,
                             ResponseCallback respond)
    {
        provisioned = uuid + QLatin1Char('|') + params["accountName"].toString();
        respond(HttpResponse::json(QJsonObject{{"status", "ok"}}, 200));
    }

    void handleTeardownSeat(const QString& uuid, const QString& seatId, ResponseCallback respond)
    {
        tornDown = uuid + QLatin1Char('|') + seatId;
        respond(HttpResponse::json(QJsonObject{{"status", "ok"}}, 200));
    }

    void handleReleaseSeatOwner(const QString& uuid, const QString& seatId,
                                ResponseCallback respond)
    {
        released = uuid + QLatin1Char('|') + seatId;
        respond(HttpResponse::json(QJsonObject{{"status", "ok"}}, 200));
    }
};

HttpResponse sendTo(const RestRouter& router, const QString& method, const QString& path,
                    bool admin, const QByteArray& body = {})
{
    HttpRequest req;
    req.method = method;
    req.path = path;
    req.body = body;
    req.isLocal = admin;
    HttpResponse reply;
    router.dispatchAsync(req, [&reply](HttpResponse resp) { reply = std::move(resp); });
    return reply;
}

HttpResponse send(const RestRouter& router, const QString& method, bool admin,
                  const QByteArray& body = {})
{
    return sendTo(router, method, QStringLiteral("/api/hosts/uuid-1/backend"), admin, body);
}

/// The host list and its merge as the server has them; only the network is
/// replaced, by what each address answers.
struct FakeHosts
{
    QMap<QString, QString> answers; // "address:port" -> serverinfo
    QMap<QString, NvComputer*> known;
    int saves = 0;

    ~FakeHosts() { qDeleteAll(known); }

    NvComputer* findHostByUuid(const QString& uuid) const { return known.value(uuid, nullptr); }

    void saveHosts() { ++saves; }

    void addOrUpdateHost(const QString& serverInfo, const NvAddress& address)
    {
        const NvComputer answer(serverInfo, address);
        if (NvComputer* existing = findHostByUuid(answer.uuid)) {
            if (existing->update(answer)) saveHosts();
        } else {
            known[answer.uuid] = new NvComputer(answer);
            saveHosts();
        }
    }

    std::pair<int, QJsonObject> handleAddManualHost(const QString& address, bool mayMoveHost)
    {
        const int colon = address.lastIndexOf(QLatin1Char(':'));
        return mergeManualHost(*this, answers.value(address),
                               NvAddress(address.left(colon), address.mid(colon + 1).toUShort()),
                               mayMoveHost);
    }
};

HttpResponse addHost(const RestRouter& router, const QString& address, bool admin)
{
    return sendTo(router, "POST", QStringLiteral("/api/hosts/manual"), admin,
                  QJsonDocument(QJsonObject{{"address", address}}).toJson());
}

QJsonObject firstHost(const HttpResponse& reply)
{
    return QJsonDocument::fromJson(reply.body).object()["hosts"].toArray().first().toObject();
}

QString serverInfo(const char* uuid, const char* localIp)
{
    return QStringLiteral("<root status_code=\"200\"><hostname>Tower</hostname>"
                          "<uniqueid>%1</uniqueid><LocalIP>%2</LocalIP></root>")
        .arg(QLatin1String(uuid), QLatin1String(localIp));
}

const QByteArray kFullBody = R"({"type":"wolf","apiUrl":"http://192.0.2.1:8080","apiToken":"t",)"
                             R"("pairUser":"u","pairPassword":"p"})";

} // namespace

void run_host_backend_writes_tests()
{
    SECTION("Host backend writes");

    RestRouter router;
    FakeManager manager;
    registerHostBackendWrites(router, manager);

    // Without the administrator's rights, nothing reaches the manager.
    CHECK_EQ(send(router, "POST", false, kFullBody).statusCode, 403);
    CHECK(manager.setCall.isEmpty());
    CHECK_EQ(send(router, "DELETE", false).statusCode, 403);
    CHECK(manager.cleared.isEmpty());

    // Refused before the body is looked at: the answer says nothing about it.
    CHECK_EQ(send(router, "POST", false, "{}").statusCode, 403);

    // The administrator gets through, every field intact.
    CHECK_EQ(send(router, "POST", true, kFullBody).statusCode, 200);
    CHECK_EQ(manager.setCall.join(QLatin1Char('|')),
             QStringLiteral("uuid-1|wolf|http://192.0.2.1:8080|t|u|p"));
    CHECK_EQ(send(router, "DELETE", true).statusCode, 200);
    CHECK_EQ(manager.cleared, QStringLiteral("uuid-1"));

    // The administrator's own mistakes are still answered as such.
    CHECK_EQ(send(router, "POST", true, "{}").statusCode, 400);
    CHECK_EQ(send(router, "POST", true, R"({"type":"wolf"})").statusCode, 400);

    const QString seats = QStringLiteral("/api/hosts/uuid-1/seats");
    const QString seat = seats + QStringLiteral("/seat-2");
    const QString owner = seat + QStringLiteral("/owner");
    const QByteArray seatBody = R"({"accountName":"ada"})";

    // The same for the seats, each route on its own.
    CHECK_EQ(sendTo(router, "POST", seats, false, seatBody).statusCode, 403);
    CHECK(manager.provisioned.isEmpty());
    CHECK_EQ(sendTo(router, "DELETE", seat, false).statusCode, 403);
    CHECK(manager.tornDown.isEmpty());
    CHECK_EQ(sendTo(router, "DELETE", owner, false).statusCode, 403);
    CHECK(manager.released.isEmpty());

    CHECK_EQ(sendTo(router, "POST", seats, true, seatBody).statusCode, 200);
    CHECK_EQ(manager.provisioned, QStringLiteral("uuid-1|ada"));
    CHECK_EQ(sendTo(router, "DELETE", seat, true).statusCode, 200);
    CHECK_EQ(manager.tornDown, QStringLiteral("uuid-1|seat-2"));
    CHECK_EQ(sendTo(router, "DELETE", owner, true).statusCode, 200);
    CHECK_EQ(manager.released, QStringLiteral("uuid-1|seat-2"));

    SECTION("Adding a host by address");

    // A browser that paired with a PIN, behind Docker's bridge: its session lets
    // it through, without the administrator's rights.
    {
        RequestGuard::Request r;
        r.method = "POST";
        r.path = "/api/hosts/manual";
        r.headers = {{"sec-fetch-site", "same-origin"},
                     {"origin", "http://192.0.2.200:8080"},
                     {"host", "192.0.2.200:8080"},
                     {"content-type", "application/json"}};
        r.bodySize = 2;
        const RequestGuard::Decision d = RequestGuard::evaluate(r, RequestGuard::Context{});
        CHECK(d.outcome == RequestGuard::Outcome::Allow);
        CHECK(!d.adminPrivilege);
    }

    RestRouter addRouter;
    FakeHosts hosts;
    registerHostManualAdd(addRouter, hosts);

    const QString home = QStringLiteral("192.0.2.10:47989");
    const QString homeLan = QStringLiteral("192.0.2.11:47989");
    const QString homeOtherPort = QStringLiteral("192.0.2.10:48000");
    const QString elsewhere = QStringLiteral("198.51.100.7:47989");
    const QString neighbour = QStringLiteral("192.0.2.20:47989");
    hosts.answers[home] = serverInfo("uuid-a", "192.0.2.11");
    hosts.answers[homeLan] = serverInfo("uuid-a", "192.0.2.11");
    hosts.answers[homeOtherPort] = serverInfo("uuid-a", "192.0.2.11");
    hosts.answers[elsewhere] = serverInfo("uuid-a", "198.51.100.7");
    hosts.answers[neighbour] = serverInfo("uuid-b", "192.0.2.20");

    CHECK_EQ(sendTo(addRouter, "POST", QStringLiteral("/api/hosts/manual"), false, "{}").statusCode,
             400);

    // The "Add computer" of that browser: a new host joins the list, and stays.
    const HttpResponse added = addHost(addRouter, home, false);
    CHECK_EQ(added.statusCode, 200);
    CHECK_EQ(firstHost(added)["uuid"].toString(), QStringLiteral("uuid-a"));
    NvComputer* tower = hosts.findHostByUuid(QStringLiteral("uuid-a"));
    CHECK(tower != nullptr);
    if (!tower) return;
    CHECK_EQ(tower->activeAddress.toString(), home);
    CHECK_EQ(tower->manualAddress.toString(), home);

    // Typing it again, or another address it reports for itself, changes nothing
    // it should not.
    CHECK_EQ(addHost(addRouter, home, false).statusCode, 200);
    CHECK_EQ(tower->activeAddress.toString(), home);
    CHECK_EQ(addHost(addRouter, homeLan, false).statusCode, 200);
    CHECK_EQ(tower->activeAddress.toString(), homeLan);

    // Another host beside it is just as welcome.
    CHECK_EQ(addHost(addRouter, neighbour, false).statusCode, 200);
    NvComputer* beside = hosts.findHostByUuid(QStringLiteral("uuid-b"));
    CHECK(beside != nullptr);
    if (!beside) return;

    // The manual address is the operator's. A session gives it to a host it adds...
    CHECK_EQ(beside->manualAddress.toString(), neighbour);

    // ...and not to one already known: same answer, nothing written...
    beside->manualAddress = NvAddress();
    const int savesKnown = hosts.saves;
    const HttpResponse named = addHost(addRouter, neighbour, false);
    CHECK_EQ(named.statusCode, 200);
    CHECK_EQ(firstHost(named)["uuid"].toString(), QStringLiteral("uuid-b"));
    CHECK(beside->manualAddress.isNull());
    CHECK_EQ(hosts.saves, savesKnown);
    // ...nor over the one it has...
    CHECK_EQ(addHost(addRouter, homeLan, false).statusCode, 200);
    CHECK_EQ(tower->manualAddress.toString(), home);

    // ...while the administrator still writes it on a known host.
    CHECK_EQ(addHost(addRouter, neighbour, true).statusCode, 200);
    CHECK_EQ(beside->manualAddress.toString(), neighbour);

    // A known host does not move, not even by a port, and nothing of the answer
    // is kept.
    const int savesBefore = hosts.saves;
    for (const QString& away : {elsewhere, homeOtherPort}) {
        const HttpResponse refused = addHost(addRouter, away, false);
        CHECK_EQ(refused.statusCode, 409);
        CHECK(!QJsonDocument::fromJson(refused.body).object().contains("hosts"));
        CHECK_EQ(tower->activeAddress.toString(), homeLan);
        CHECK_EQ(tower->manualAddress.toString(), home);
        CHECK_EQ(tower->localAddress.toString(), homeLan);
    }
    CHECK_EQ(hosts.saves, savesBefore);

    // The answer shows a host's backend as the host list does.
    tower->backendApiUrl = QStringLiteral("http://192.0.2.30:8080");
    tower->backendPairUser = QStringLiteral("ada");
    const QJsonObject asSession = firstHost(addHost(addRouter, homeLan, false));
    CHECK_EQ(asSession["uuid"].toString(), QStringLiteral("uuid-a"));
    CHECK(!asSession.contains("backendApiUrl"));
    CHECK(!asSession.contains("backendPairUser"));
    const QJsonObject asAdmin = firstHost(addHost(addRouter, homeLan, true));
    CHECK_EQ(asAdmin["backendApiUrl"].toString(), QStringLiteral("http://192.0.2.30:8080"));
    CHECK_EQ(asAdmin["backendPairUser"].toString(), QStringLiteral("ada"));

    // Once paired, even an address it has does not switch it: the poll does, on
    // proof. The answer still shows the host.
    tower->serverCertPem = QByteArrayLiteral("paired");
    tower->manualAddress = NvAddress(QStringLiteral("192.0.2.10"), 47989);
    const HttpResponse paired = addHost(addRouter, home, false);
    CHECK_EQ(paired.statusCode, 200);
    CHECK_EQ(firstHost(paired)["uuid"].toString(), QStringLiteral("uuid-a"));
    CHECK_EQ(tower->activeAddress.toString(), homeLan);
    tower->serverCertPem.clear();

    // The administrator can move it.
    CHECK_EQ(addHost(addRouter, elsewhere, true).statusCode, 200);
    CHECK_EQ(tower->activeAddress.toString(), elsewhere);
    CHECK_EQ(tower->manualAddress.toString(), elsewhere);
}
