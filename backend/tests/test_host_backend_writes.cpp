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
};

HttpResponse send(const RestRouter& router, const QString& method, bool admin,
                  const QByteArray& body = {})
{
    HttpRequest req;
    req.method = method;
    req.path = QStringLiteral("/api/hosts/uuid-1/backend");
    req.body = body;
    req.isLocal = admin;
    HttpResponse reply;
    router.dispatchAsync(req, [&reply](HttpResponse resp) { reply = std::move(resp); });
    return reply;
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
}
