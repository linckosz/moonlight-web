/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "server/RestRouter.h"

#include <QJsonObject>

// Synchronously dispatch and capture the response.
static HttpResponse call(const RestRouter& r, const QString& method, const QString& path,
                         const QByteArray& body = {})
{
    HttpRequest req;
    req.method = method;
    req.path = path;
    req.body = body;
    HttpResponse captured;
    r.dispatchAsync(req, [&captured](HttpResponse resp) { captured = resp; });
    return captured;
}

void run_rest_router_tests()
{
    SECTION("RestRouter");

    RestRouter router;

    router.get("/api/ping", [](const HttpRequest&) { return HttpResponse::text("pong"); });
    router.post("/api/echo", [](const HttpRequest& req) {
        HttpResponse r;
        r.statusCode = 201;
        r.body = req.body;
        return r;
    });
    router.del("/api/hosts/:id", [](const HttpRequest& req) {
        QJsonObject o;
        o["deleted"] = req.pathParams.value("id");
        return HttpResponse::json(o);
    });
    router.getAsync("/api/hosts/:id/apps", [](const HttpRequest& req, const ResponseCallback& cb) {
        cb(HttpResponse::text(req.pathParams.value("id")));
    });

    // Exact GET route.
    CHECK_EQ(call(router, "GET", "/api/ping").statusCode, 200);
    CHECK_EQ(QString::fromUtf8(call(router, "GET", "/api/ping").body), QString("pong"));

    // POST echoes the body with a custom status.
    HttpResponse echo = call(router, "POST", "/api/echo", QByteArray("data"));
    CHECK_EQ(echo.statusCode, 201);
    CHECK_EQ(QString::fromUtf8(echo.body), QString("data"));

    // Parameterized DELETE extracts :id.
    HttpResponse del = call(router, "DELETE", "/api/hosts/abc123");
    CHECK_EQ(del.statusCode, 200);
    CHECK(del.body.contains("abc123"));

    // Parameterized async GET extracts :id.
    CHECK_EQ(QString::fromUtf8(call(router, "GET", "/api/hosts/42/apps").body), QString("42"));

    // hasRoute reflects both exact and param routes.
    CHECK(router.hasRoute("GET", "/api/ping"));
    CHECK(router.hasRoute("GET", "/api/hosts/7/apps"));
    CHECK(!router.hasRoute("GET", "/api/missing"));
    CHECK(!router.hasRoute("PUT", "/api/ping")); // wrong method

    // Unknown path → 404 with a JSON error body.
    HttpResponse nf = call(router, "GET", "/api/missing");
    CHECK_EQ(nf.statusCode, 404);
    CHECK(nf.body.contains("error"));
}
