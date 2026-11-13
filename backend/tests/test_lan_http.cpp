/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Plain HTTP for the devices the owner listed (issue #42): who arrives that
 * way, under which name, from which page, what it may ask, and with which key.
 */

#include "test_framework.h"
#include "server/LanHttp.h"

#include <QList>
#include <QMap>
#include <QPair>

void run_lan_http_tests()
{
    using namespace LanHttp;

    const QSet<QString> own = {QStringLiteral("192.168.1.10"), QStringLiteral("127.0.0.1"),
                               QStringLiteral("::1"), QStringLiteral("fe80::1%eth0")};
    const QSet<QString> ownIpv4 = {QStringLiteral("192.168.1.10"), QStringLiteral("10.0.0.5")};

    SECTION("LanHttp — the peer, as IPv4 only");

    CHECK_EQ(normalizePeer("192.168.1.50"), QString("192.168.1.50"));
    CHECK_EQ(normalizePeer("::ffff:192.168.1.50"), QString("192.168.1.50"));
    CHECK_EQ(normalizePeer("::FFFF:192.168.1.50"), QString("192.168.1.50"));
    CHECK_EQ(normalizePeer(" 192.168.1.50 "), QString("192.168.1.50"));
    // IPv6 is not served in this version (LH2), whatever its range.
    CHECK(normalizePeer("fe80::1").isEmpty());
    CHECK(normalizePeer("fd00::50").isEmpty());
    CHECK(normalizePeer("fe80::1%eth0").isEmpty());
    CHECK(normalizePeer("::ffff:c0a8:0132").isEmpty());
    // Forms a person would not read as the same address.
    CHECK(normalizePeer("192.168.001.050").isEmpty());
    CHECK(normalizePeer("192.168.1").isEmpty());
    CHECK(normalizePeer("3232235826").isEmpty());
    CHECK(normalizePeer("tv.local").isEmpty());
    CHECK(normalizePeer("").isEmpty());

    SECTION("LanHttp — private ranges");

    CHECK(isPrivateIpv4("10.1.2.3"));
    CHECK(isPrivateIpv4("172.16.0.1"));
    CHECK(isPrivateIpv4("172.31.255.254"));
    CHECK(isPrivateIpv4("192.168.0.1"));
    CHECK(isPrivateIpv4("169.254.10.20"));
    CHECK(!isPrivateIpv4("172.32.0.1"));
    CHECK(!isPrivateIpv4("127.0.0.1"));
    CHECK(!isPrivateIpv4("100.64.1.1")); // carrier-grade NAT: where mesh VPNs live
    CHECK(!isPrivateIpv4("8.8.8.8"));
    CHECK(!isPrivateIpv4("0.0.0.0"));
    CHECK(!isPrivateIpv4("255.255.255.255"));

    SECTION("LanHttp — the list as honoured");

    {
        QStringList dropped;
        const QSet<QString> usable = usableClients(
            {"192.168.1.50", " 192.168.1.51", "::ffff:192.168.1.52", "192.168.1.10", "127.0.0.1",
             "8.8.8.8", "100.64.0.9", "fd00::50", "tv.local", "", "192.168.1.050"},
            own, &dropped);
        CHECK_EQ(usable.size(), qsizetype(3));
        CHECK(usable.contains("192.168.1.50"));
        CHECK(usable.contains("192.168.1.51"));
        CHECK(usable.contains("192.168.1.52"));
        // This machine's own address is never "the TV": another account of this
        // very PC would otherwise arrive as one.
        CHECK(!usable.contains("192.168.1.10"));
        CHECK_EQ(dropped.size(), qsizetype(8));
        CHECK(dropped.contains("192.168.1.10"));
    }

    SECTION("LanHttp — who arrives on plain HTTP");

    {
        const QSet<QString> usable = {QStringLiteral("192.168.1.50")};
        CHECK(admits("192.168.1.50", usable, own));
        CHECK(admits("::ffff:192.168.1.50", usable, own));
        CHECK(!admits("192.168.1.51", usable, own));
        CHECK(!admits("127.0.0.1", usable, own));
        CHECK(!admits("", usable, own));
        // A list that somehow holds this machine's address (written by hand,
        // or an address the machine took since) still never admits it.
        const QSet<QString> mine = {QStringLiteral("192.168.1.10")};
        CHECK(!admits("192.168.1.10", mine, own));
        CHECK(!admits("::ffff:192.168.1.10", mine, own));
        // Nor anything that is not private, even if the set was built by hand.
        const QSet<QString> pub = {QStringLiteral("8.8.8.8")};
        CHECK(!admits("8.8.8.8", pub, own));
    }

    SECTION("LanHttp — the name the page asked for");

    CHECK_EQ(validatedHost("192.168.1.10:8080", ownIpv4, "dualrtx.local", 8080),
             QString("192.168.1.10:8080"));
    CHECK_EQ(validatedHost("10.0.0.5:8080", ownIpv4, "dualrtx.local", 8080),
             QString("10.0.0.5:8080"));
    CHECK_EQ(validatedHost("DualRTX.local:8080", ownIpv4, "dualrtx.local", 8080),
             QString("dualrtx.local:8080"));
    CHECK_EQ(validatedHost("dualrtx.local:8080", ownIpv4, "DualRTX.local", 8080),
             QString("dualrtx.local:8080"));
    // The default port is left out by the browser, and by the answer.
    CHECK_EQ(validatedHost("192.168.1.10", ownIpv4, "dualrtx.local", 80), QString("192.168.1.10"));
    CHECK_EQ(validatedHost("192.168.1.10:80", ownIpv4, "dualrtx.local", 80),
             QString("192.168.1.10"));
    // Another port, or none when the port is not 80.
    CHECK(validatedHost("192.168.1.10:8443", ownIpv4, "dualrtx.local", 8080).isEmpty());
    CHECK(validatedHost("192.168.1.10", ownIpv4, "dualrtx.local", 8080).isEmpty());
    CHECK(validatedHost("192.168.1.10:08080", ownIpv4, "dualrtx.local", 8080).isEmpty());
    CHECK(validatedHost("192.168.1.10:", ownIpv4, "dualrtx.local", 8080).isEmpty());
    // Not one of this machine's addresses: a rebound name, or another host.
    CHECK(validatedHost("192.168.1.11:8080", ownIpv4, "dualrtx.local", 8080).isEmpty());
    CHECK(validatedHost("evil.example:8080", ownIpv4, "dualrtx.local", 8080).isEmpty());
    CHECK(validatedHost("other.local:8080", ownIpv4, "dualrtx.local", 8080).isEmpty());
    CHECK(
        validatedHost("dualrtx.local.evil.example:8080", ownIpv4, "dualrtx.local", 8080).isEmpty());
    CHECK(validatedHost("localhost:8080", ownIpv4, "dualrtx.local", 8080).isEmpty());
    CHECK(validatedHost("127.0.0.1:8080", {"127.0.0.1"}, "dualrtx.local", 8080).isEmpty());
    // IPv6 literals, zones, credentials and paths are not served here.
    CHECK(validatedHost("[fe80::1%25eth0]:8080", ownIpv4, "dualrtx.local", 8080).isEmpty());
    CHECK(validatedHost("[::1]:8080", ownIpv4, "dualrtx.local", 8080).isEmpty());
    CHECK(validatedHost("user@192.168.1.10:8080", ownIpv4, "dualrtx.local", 8080).isEmpty());
    CHECK(validatedHost("192.168.1.10:8080/x", ownIpv4, "dualrtx.local", 8080).isEmpty());
    CHECK(validatedHost("", ownIpv4, "dualrtx.local", 8080).isEmpty());
    CHECK(validatedHost("192.168.1.10:8080", ownIpv4, "dualrtx.local", 0).isEmpty());
    // A name that is not a .local one is never accepted, even if it is ours.
    CHECK(validatedHost("dualrtx:8080", ownIpv4, "dualrtx", 8080).isEmpty());

    SECTION("LanHttp — the page that asks");

    CHECK(originAllowed("http://192.168.1.10:8080", "192.168.1.10:8080"));
    CHECK(originAllowed("HTTP://192.168.1.10:8080", "192.168.1.10:8080"));
    CHECK(originAllowed("http://192.168.1.10", "192.168.1.10"));
    // Required here, unlike on HTTPS.
    CHECK(!originAllowed("", "192.168.1.10:8080"));
    CHECK(!originAllowed("null", "192.168.1.10:8080"));
    CHECK(!originAllowed("https://192.168.1.10:8080", "192.168.1.10:8080"));
    CHECK(!originAllowed("http://192.168.1.10:8443", "192.168.1.10:8080"));
    CHECK(!originAllowed("http://192.168.1.10:8080/", "192.168.1.10:8080"));
    CHECK(!originAllowed("http://dualrtx.local:8080", "192.168.1.10:8080"));
    CHECK(!originAllowed("http://192.168.1.10:8080", ""));

    SECTION("LanHttp — the routes a TV may ask");

    // What it needs.
    CHECK(route("GET", "/"));
    CHECK(route("GET", "/index.html"));
    CHECK(route("HEAD", "/js/app.js"));
    CHECK(route("GET", "/version.json"));
    CHECK(route("GET", "/api/health"));
    CHECK(route("GET", "/api/hosts"));
    CHECK(route("GET", "/api/hosts/abc-123/apps"));
    CHECK(route("POST", "/api/hosts/abc-123/start"));
    CHECK(route("POST", "/api/hosts/abc-123/quit"));
    CHECK(route("GET", "/api/hosts/abc-123/stream-state"));
    CHECK(route("GET", "/api/lan-http/status"));
    CHECK(route("POST", "/api/lan-http/pair"));
    CHECK(route("GET", "/api/lan-http/pair/0123456789abcdef"));
    // Signing a TV out is done in Active Sessions, not by the TV.
    CHECK(!route("POST", "/api/lan-http/logout"));
    // Static files are read, never written.
    CHECK(!route("POST", "/index.html"));
    CHECK(!route("PUT", "/js/app.js"));
    // The wrong verb on a listed path.
    CHECK(!route("POST", "/api/hosts"));
    CHECK(!route("GET", "/api/hosts/abc-123/start"));
    CHECK(!route("POST", "/api/settings/streaming"));
    // One segment for one star, never none and never two.
    CHECK(!route("GET", "/api/hosts//apps"));
    CHECK(!route("GET", "/api/hosts/a/b/apps"));
    CHECK(!route("GET", "/api/lan-http/pair/"));
    // Everything a TV must never reach, named so that a change here is seen.
    CHECK(!route("GET", "/api"));
    CHECK(!route("GET", "/api/admin/token"));
    CHECK(!route("GET", "/api/admin/settings"));
    CHECK(!route("POST", "/api/admin/settings"));
    CHECK(!route("POST", "/api/admin/pin/generate"));
    CHECK(!route("GET", "/api/app/manifest"));
    CHECK(!route("GET", "/api/app/web-manifest"));
    CHECK(!route("GET", "/api/auth/status"));
    CHECK(!route("POST", "/api/auth/validate"));
    CHECK(!route("GET", "/api/auth/sessions"));
    CHECK(!route("POST", "/api/auth/sessions/revoke"));
    CHECK(!route("POST", "/api/auth/host-key"));
    CHECK(!route("POST", "/api/auth/pair-request"));
    CHECK(!route("POST", "/api/auth/pair-request/abc/approve"));
    CHECK(!route("GET", "/api/server/remote-link"));
    CHECK(!route("GET", "/api/server/status"));
    CHECK(!route("GET", "/api/server/stream-activity"));
    CHECK(!route("POST", "/api/server/stream-disconnect"));
    CHECK(!route("POST", "/api/hosts/abc-123/stop-session"));
    CHECK(!route("POST", "/api/hosts/abc-123/pair"));
    CHECK(!route("POST", "/api/hosts/manual"));
    CHECK(!route("POST", "/api/hosts/scan"));
    CHECK(!route("POST", "/api/hosts/abc-123/wol"));
    CHECK(!route("POST", "/api/share/slots/2/activate"));
    CHECK(!route("POST", "/api/share/player/join"));
    CHECK(!route("GET", "/api/logs/archive"));
    CHECK(!route("POST", "/api/update/start"));
    CHECK(!route("POST", "/api/internet/enable"));
    CHECK(!route("POST", "/api/system/quit"));
    CHECK(!route("POST", "/api/lan-http/approve/abc"));
    CHECK(!route("GET", "/api/lan-http/approve/abc"));
    CHECK(!route("GET", "/api/admin/connection-history"));
    CHECK(!route("GET", ""));
    CHECK(!route("GET", "api/hosts"));

    SECTION("LanHttp — the stream sockets a TV may open");

    CHECK_EQ(webSocketSlot("/ws"), 0);
    CHECK_EQ(webSocketSlot("/ws1"), 1);
    CHECK_EQ(webSocketSlot("/ws7"), 7);
    CHECK_EQ(webSocketSlot("/ws123"), 123);
    // Never the legacy video relay, never the control channel.
    CHECK_EQ(webSocketSlot("/ws/stream"), -1);
    CHECK_EQ(webSocketSlot("/ws3/stream"), -1);
    CHECK_EQ(webSocketSlot("/ws/control"), -1);
    CHECK_EQ(webSocketSlot("/ws01"), -1);
    CHECK_EQ(webSocketSlot("/ws1234"), -1);
    CHECK_EQ(webSocketSlot("/ws-1"), -1);
    CHECK_EQ(webSocketSlot("/ws1/"), -1);
    CHECK_EQ(webSocketSlot("/wss"), -1);
    CHECK_EQ(webSocketSlot("/"), -1);

    SECTION("LanHttp — the policy of an answer");

    {
        const QString csp = securityPolicy("192.168.1.10:8080");
        CHECK(csp.contains("connect-src 'self' ws://192.168.1.10:8080;"));
        CHECK(!csp.contains("wss://"));
        CHECK(!csp.contains("upgrade-insecure-requests"));
        CHECK(csp.contains("frame-ancestors 'none'"));
        CHECK(securityPolicy("").contains("connect-src 'self';"));
    }

    SECTION("LanHttp — the cookie");

    {
        const QString c = cookie("abc123", 2592000);
        CHECK(c.startsWith("mw_lan_http=abc123;"));
        CHECK(c.contains("HttpOnly"));
        CHECK(c.contains("SameSite=Strict"));
        CHECK(c.contains("Path=/"));
        CHECK(c.contains("Max-Age=2592000"));
        // The browser refuses a Secure cookie over http://.
        CHECK(!c.contains("Secure"));
        CHECK(cookie("abc", -5).contains("Max-Age=0"));
        const QString cleared = clearedCookie();
        CHECK(cleared.startsWith("mw_lan_http=;"));
        CHECK(cleared.contains("Max-Age=0"));
        CHECK(!cleared.contains("Secure"));
    }

    SECTION("LanHttp — reading the cookie");

    CHECK_EQ(tokenFromCookieHeader("mw_lan_http=abc"), QString("abc"));
    CHECK_EQ(tokenFromCookieHeader("a=1; mw_lan_http=abc; b=2"), QString("abc"));
    CHECK_EQ(tokenFromCookieHeader("a=1;mw_lan_http=abc"), QString("abc"));
    // Two of them: neither is read.
    CHECK(tokenFromCookieHeader("mw_lan_http=abc; mw_lan_http=def").isEmpty());
    // The HTTPS cookies are not this one.
    CHECK(tokenFromCookieHeader("mw_session=abc; mw_player=def").isEmpty());
    CHECK(tokenFromCookieHeader("MW_LAN_HTTP=abc").isEmpty());
    CHECK(tokenFromCookieHeader("xmw_lan_http=abc").isEmpty());
    CHECK(tokenFromCookieHeader("").isEmpty());
    SECTION("LanHttp — the list of routes, as it stands");

    // Exactly this list: a route added to LanHttp.cpp shows in this test's diff.
    CHECK_EQ(routeTable(),
             QStringList({"GET /api/health", "GET /api/server/hostname", "GET /api/hosts",
                          "GET /api/hosts/*/apps", "GET /api/hosts/*/running-app",
                          "GET /api/hosts/*/stream-state", "POST /api/hosts/*/start",
                          "POST /api/hosts/*/quit", "GET /api/settings/streaming",
                          "GET /api/lan-http/status", "POST /api/lan-http/pair",
                          "GET /api/lan-http/pair/*"}));
    CHECK(openRoute("/api/health"));
    CHECK(openRoute("/api/lan-http/status"));
    CHECK(openRoute("/api/lan-http/pair"));
    CHECK(openRoute("/api/lan-http/pair/0123456789abcdef"));
    CHECK(!openRoute("/api/hosts"));
    CHECK(!openRoute("/api/lan-http/pair/a/b"));

    SECTION("LanHttp — the gate of a request, end to end");

    using Headers = QMap<QString, QString>;
    const QString host = QStringLiteral("192.168.1.10:8080");
    const QString origin = QStringLiteral("http://192.168.1.10:8080");
    {
        // a) The HTTPS cookies sent alongside never reach a route.
        const Headers h{{"cookie", "mw_session=S; mw_player=P; mw_lan_http=T; other=1"}};
        const Gate g = gate("GET", "/api/hosts", h, host, Key::Valid, false);
        CHECK(g.verdict == Verdict::Serve);
        CHECK_EQ(g.cookieHeader, QStringLiteral("mw_lan_http=T"));
        const Gate none =
            gate("GET", "/api/hosts", Headers{{"cookie", "mw_session=S"}}, host, Key::None, false);
        CHECK(none.cookieHeader.isEmpty());
        CHECK(gate("GET", "/", Headers{{"cookie", "mw_session=S"}}, host, Key::None, false)
                  .cookieHeader.isEmpty());
    }
    {
        // b) A write without an Origin, or with another one: 403, counted.
        const Headers withCookie{{"cookie", "mw_lan_http=T"}};
        Gate g = gate("POST", "/api/hosts/h/start", withCookie, host, Key::Valid, false);
        CHECK(g.verdict == Verdict::Forbidden);
        CHECK(g.counted);
        Headers foreign = withCookie;
        foreign["origin"] = "http://evil.example";
        g = gate("POST", "/api/hosts/h/start", foreign, host, Key::Valid, false);
        CHECK(g.verdict == Verdict::Forbidden);
        Headers https = withCookie;
        https["origin"] = "https://192.168.1.10:8080";
        CHECK(gate("POST", "/api/hosts/h/start", https, host, Key::Valid, false).verdict ==
              Verdict::Forbidden);
        Headers mine = withCookie;
        mine["origin"] = origin;
        CHECK(gate("POST", "/api/hosts/h/start", mine, host, Key::Valid, false).verdict ==
              Verdict::Serve);
        // RequestGuard's refusal (another site's page, a body that is not JSON).
        g = gate("GET", "/api/hosts", withCookie, host, Key::Valid, true);
        CHECK(g.verdict == Verdict::Forbidden);
        CHECK(g.counted);
    }
    {
        // c) A route not on the list: 404, even with a good key, never counted.
        Headers h{{"cookie", "mw_lan_http=T"}, {"origin", origin}};
        for (const auto& [method, path] :
             QList<QPair<QString, QString>>{{"GET", "/api/admin/settings"},
                                            {"POST", "/api/hosts/h/stop-session"},
                                            {"GET", "/api/auth/status"},
                                            {"POST", "/api/lan-http/approve/x"},
                                            {"GET", "/api/server/stream-activity"}}) {
            const Gate g = gate(method, path, h, host, Key::Valid, false);
            CHECK(g.verdict == Verdict::NotFound);
            CHECK(!g.counted);
        }
    }
    {
        // C3: a GET without a good key is refused, and counted only when the
        // key is another address's: a TV whose session ran out keeps asking
        // /api/hosts, and must still reach the route that lets it in again.
        const Headers h{{"cookie", "mw_lan_http=T"}};
        Gate g = gate("GET", "/api/hosts", Headers{}, host, Key::None, false);
        CHECK(g.verdict == Verdict::Unauthorized);
        CHECK(!g.counted);
        g = gate("GET", "/api/hosts", h, host, Key::Expired, false);
        CHECK(g.verdict == Verdict::Unauthorized);
        CHECK(!g.counted);
        g = gate("GET", "/api/hosts", h, host, Key::OtherAddress, false);
        CHECK(g.verdict == Verdict::Unauthorized);
        CHECK(g.counted);
        // A write without a good key: counted.
        Headers w = h;
        w["origin"] = origin;
        g = gate("POST", "/api/hosts/h/start", w, host, Key::Expired, false);
        CHECK(g.verdict == Verdict::Unauthorized);
        CHECK(g.counted);
        // The open routes need no key; static files neither.
        CHECK(gate("GET", "/api/lan-http/status", Headers{}, host, Key::None, false).verdict ==
              Verdict::Serve);
        Headers pair{{"origin", origin}};
        CHECK(gate("POST", "/api/lan-http/pair", pair, host, Key::Expired, false).verdict ==
              Verdict::Serve);
        CHECK(gate("GET", "/js/app.js", Headers{}, host, Key::None, false).verdict ==
              Verdict::Serve);
    }

    SECTION("LanHttp — the gate of the stream socket");

    {
        const auto own = [](int slot) { return slot == 0; };
        // e) No Origin, or another: 403, counted.
        WsGate g = webSocketGate("", host, "/ws", Key::Valid, own);
        CHECK_EQ(g.status, 403);
        CHECK(g.counted);
        CHECK_EQ(webSocketGate("http://evil.example", host, "/ws", Key::Valid, own).status, 403);
        // Another device's slot: 403.
        g = webSocketGate(origin, host, "/ws1", Key::Valid, own);
        CHECK_EQ(g.status, 403);
        CHECK(g.counted);
        // The legacy relay and the control channel: 404.
        CHECK_EQ(webSocketGate(origin, host, "/ws1/stream", Key::Valid, own).status, 404);
        CHECK_EQ(webSocketGate(origin, host, "/ws/stream", Key::Valid, own).status, 404);
        CHECK_EQ(webSocketGate(origin, host, "/ws/control", Key::Valid, own).status, 404);
        // Without a good key: 401, counted, and the slot never asked.
        bool asked = false;
        g = webSocketGate(origin, host, "/ws", Key::Expired, [&asked](int) {
            asked = true;
            return true;
        });
        CHECK_EQ(g.status, 401);
        CHECK(g.counted);
        CHECK(!asked);
        // Its own slot, with its key: open.
        g = webSocketGate(origin, host, "/ws", Key::Valid, own);
        CHECK_EQ(g.status, 0);
        CHECK_EQ(g.slot, 0);
        CHECK_EQ(webSocketGate(origin, host, "/ws", Key::Valid, {}).status, 403);
    }

    SECTION("LanHttp — a path as the log shows it");

    CHECK_EQ(pathForLog("/api/hosts"), QStringLiteral("/api/hosts"));
    CHECK_EQ(pathForLog("/api/lan-http/pair/AbCdEfGhIjKlMnOp"),
             QStringLiteral("/api/lan-http/pair/*"));
    CHECK_EQ(pathForLog("/api/hosts/3f2a-11/start"), QStringLiteral("/api/hosts/*/start"));
    CHECK_EQ(pathForLog("/js/app.js"), QStringLiteral("/js/*"));
    CHECK_EQ(pathForLog("/api/abcdefghijklmnopqrstuvwxy"), QStringLiteral("/api/*"));
    CHECK_EQ(pathForLog("/"), QStringLiteral("/"));
}
