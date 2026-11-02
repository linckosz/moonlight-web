/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"

#include "server/routes/HostBackendWrites.h"

#include <QJsonObject>
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
}
