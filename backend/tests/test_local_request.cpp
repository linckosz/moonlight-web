/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * `moonlightweb --local-request`: what a script of this machine may ask the
 * running server through its local channel, and what of the answer it is
 * shown. Refusals are decided before the channel is opened.
 */
#include "test_framework.h"
#include "server/LocalRequest.h"
#include "server/LocalChannel.h"
#include "server/RequestGuard.h"

#include <QJsonDocument>
#include <QJsonObject>

void run_local_request_tests()
{
    using LocalRequest::allowed;
    using LocalRequest::Asked;
    using LocalRequest::parse;
    using LocalRequest::printable;

    SECTION("local request: the option's value is one method and one exact path");
    {
        const std::optional<Asked> get = parse(QStringLiteral("GET /api/settings/streaming"));
        CHECK(get.has_value());
        if (get) {
            CHECK(get->method == QStringLiteral("GET"));
            CHECK(get->path == QStringLiteral("/api/settings/streaming"));
        }
        CHECK(parse(QStringLiteral("POST /api/admin/pin/generate")).has_value());
        // A query, a fragment, a second word, another method, no method.
        for (const char* bad :
             {"GET /api/server/remote-link?p=/admin", "GET /api/server/status#x",
              "GET /api/server/status extra", "get /api/server/status", "DELETE /api/hosts",
              "PUT /api/settings/streaming", "/api/server/status", "GET", "GET  /api/health",
              "GET api/health", "GET /api/health\n", " GET /api/health", ""})
            CHECK(!parse(QString::fromLatin1(bad)).has_value());
        // Nothing that walks out of the route's own path.
        CHECK(!parse(QStringLiteral("GET /api/../admin/token")).has_value());
        CHECK(!parse(QStringLiteral("GET /api/%2e%2e/x")).has_value());
    }

    SECTION("local request: the channel's routes, less the four that are not a script's to ask");
    {
        for (const char* ok :
             {"GET /api/health", "GET /api/native/status", "GET /api/server/status",
              "GET /api/server/stream-activity", "GET /api/setup/status", "GET /api/auth/status",
              "GET /api/internet/status", "GET /api/settings/streaming",
              "POST /api/settings/streaming", "POST /api/admin/pin/generate",
              "POST /api/system/restart", "POST /api/system/quit", "POST /api/local/focus"}) {
            const std::optional<Asked> asked = parse(QString::fromLatin1(ok));
            CHECK(asked.has_value());
            if (asked) CHECK(allowed(*asked));
        }
        // The owner's link, the admin password and the internet link: asked by
        // their own verbs, with their own rules, and not this way.
        CHECK(!allowed(Asked{QStringLiteral("GET"), QStringLiteral("/api/server/remote-link")}));
        CHECK(!allowed(Asked{QStringLiteral("POST"), QStringLiteral("/api/admin/password")}));
        CHECK(!allowed(Asked{QStringLiteral("POST"), QStringLiteral("/api/internet/enable")}));
        // The probe of the router: no credential, and nothing a script should
        // set off by asking for a status.
        CHECK(!allowed(Asked{QStringLiteral("GET"), QStringLiteral("/api/internet/upnp-probe")}));
        CHECK(RequestGuard::localChannelRoute(QStringLiteral("GET"),
                                              QStringLiteral("/api/internet/upnp-probe")));
        // The three others are routes of the channel too: it is this verb that
        // leaves them out.
        CHECK(RequestGuard::localChannelRoute(QStringLiteral("GET"),
                                              QStringLiteral("/api/server/remote-link")));
        CHECK(RequestGuard::localChannelRoute(QStringLiteral("POST"),
                                              QStringLiteral("/api/admin/password")));
        CHECK(RequestGuard::localChannelRoute(QStringLiteral("POST"),
                                              QStringLiteral("/api/internet/enable")));
        // Nothing outside the channel's list: other routes, another method on a
        // listed path, a path that only starts like one.
        for (const char* no :
             {"GET /api/admin/token", "GET /api/admin/settings", "GET /api/hosts",
              "POST /api/auth/validate", "POST /api/auth/host-key", "GET /api/auth/sessions",
              "POST /api/server/status", "GET /api/admin/pin/generate", "GET /api/system/quit",
              "GET /api/settings/streaming/x", "GET /api/healthz", "GET /", "GET /admin"}) {
            const std::optional<Asked> asked = parse(QString::fromLatin1(no));
            if (asked) CHECK(!allowed(*asked));
        }
    }

    SECTION("local request: a body is one frame of the channel at most");
    {
        CHECK_EQ(static_cast<int>(LocalRequest::maxBodyBytes()),
                 static_cast<int>(mw::localchannel::kMaxRequestBytes));
    }

    SECTION("local request: an answer is printed without the access PIN");
    {
        const Asked status{QStringLiteral("GET"), QStringLiteral("/api/auth/status")};
        const QByteArray body = R"({"authenticated":true,"pin":"12345678","pin_consumed":false,)"
                                R"("nested":{"PIN":"x","keep":1},"list":[{"pin":"y","n":2}]})";
        const QByteArray shown = printable(status, body);
        CHECK(!shown.contains("12345678"));
        const QJsonObject obj = QJsonDocument::fromJson(shown).object();
        CHECK(!obj.contains(QStringLiteral("pin")));
        // What merely has the word in its name stays.
        CHECK(obj.contains(QStringLiteral("pin_consumed")));
        CHECK(obj.value(QStringLiteral("authenticated")).toBool());
        // At any depth, whatever its case.
        CHECK(!shown.contains("\"PIN\""));
        CHECK(!shown.contains("\"pin\""));
        CHECK(shown.contains("\"keep\":1"));
        CHECK(shown.contains("\"n\":2"));

        // The request whose answer is a PIN keeps it: that is what was asked.
        const Asked generate{QStringLiteral("POST"), QStringLiteral("/api/admin/pin/generate")};
        const QByteArray made = R"({"status":"ok","pin":"87654321"})";
        CHECK(printable(generate, made) == made);
        // Only that one: the same path asked another way is not it.
        const Asked other{QStringLiteral("GET"), QStringLiteral("/api/admin/pin/generate")};
        CHECK(!printable(other, made).contains("87654321"));

        // What is not JSON is left as it is.
        CHECK(printable(status, QByteArray("plain text")) == QByteArray("plain text"));
        CHECK(printable(status, QByteArray()) == QByteArray());
    }

    SECTION("local request: nor with this machine's identifier or its public address");
    {
        const Asked internet{QStringLiteral("GET"), QStringLiteral("/api/internet/status")};
        const QByteArray body =
            R"({"active":true,"unique_id":"abc123def","public_ip":"203.0.113.7",)"
            R"("local_ip":"192.168.1.20","domain":"abc.example.test",)"
            R"("rendezvous":{"url":"https://rdv.example.test/x","online":true,"Unique_Id":"zz"},)"
            R"("list":[{"PUBLIC_IP":"198.51.100.9","n":3}]})";
        const QByteArray shown = printable(internet, body);
        // Neither, at any depth, whatever the case of the name.
        CHECK(!shown.contains("abc123def"));
        CHECK(!shown.contains("203.0.113.7"));
        CHECK(!shown.contains("198.51.100.9"));
        CHECK(!shown.contains("\"zz\""));
        const QJsonObject obj = QJsonDocument::fromJson(shown).object();
        CHECK(!obj.contains(QStringLiteral("unique_id")));
        CHECK(!obj.contains(QStringLiteral("public_ip")));
        // What a bench needs to reach the machine by stays: its LAN address,
        // and the address a browser comes in through.
        CHECK(obj.value(QStringLiteral("local_ip")).toString() == QStringLiteral("192.168.1.20"));
        CHECK(obj.value(QStringLiteral("domain")).toString() == QStringLiteral("abc.example.test"));
        CHECK(obj.value(QStringLiteral("rendezvous"))
                  .toObject()
                  .value(QStringLiteral("url"))
                  .toString() == QStringLiteral("https://rdv.example.test/x"));
        CHECK(obj.value(QStringLiteral("rendezvous"))
                  .toObject()
                  .value(QStringLiteral("online"))
                  .toBool());
        CHECK(shown.contains("\"n\":3"));
        // The same in the wizard's status, where they sit one level down.
        const Asked setup{QStringLiteral("GET"), QStringLiteral("/api/setup/status")};
        const QByteArray nested =
            R"({"internet":{"unique_id":"q1","public_ip":"203.0.113.8","phase":"up"}})";
        const QByteArray shownSetup = printable(setup, nested);
        CHECK(!shownSetup.contains("q1"));
        CHECK(!shownSetup.contains("203.0.113.8"));
        CHECK(shownSetup.contains("\"phase\":\"up\""));
    }

    SECTION("local request: the exit code says whether it was answered with a yes");
    {
        CHECK_EQ(LocalRequest::exitCodeFor(200), 0);
        CHECK_EQ(LocalRequest::exitCodeFor(204), 0);
        CHECK_EQ(LocalRequest::exitCodeFor(299), 0);
        // Refused, not found, an error, and an answer that never came.
        for (int status : {0, 199, 300, 400, 401, 403, 404, 500, 503, 504})
            CHECK_EQ(LocalRequest::exitCodeFor(status), 1);
    }
}
