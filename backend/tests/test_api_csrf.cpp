/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The CSRF gates seen from the wire. test_request_guard.cpp checks the rules in
 * isolation; this file checks that a request as it actually arrives — raw bytes,
 * through HttpParser, through the same mapping production uses — reaches those
 * rules intact and comes back with the right HTTP status.
 *
 * That seam is where a silent failure would live: the guard reads lowercase
 * header keys, so a parser that stopped normalizing them would leave every
 * check reading empty strings and open every gate, with the whole of
 * test_request_guard.cpp still green.
 */
#include "test_framework.h"

#include "server/HttpParser.h"
#include "server/RequestGuard.h"

namespace {

/// What the caller would receive: the refusal status (0 when the request goes
/// on to its route) and the privileges it carries there.
struct Reply
{
    int status = 0;
    QString error;
    bool localPrivilege = false;
    bool adminPrivilege = false;
};

/// Everything the server knows about the caller independently of the request.
/// Defaults to the interesting case — a browser on the host machine, no session
/// and no admin key — so each test only states what it changes.
struct Caller
{
    bool peerLocal = true;
    bool hostSession = false;
    bool adminSession = false;
    bool adminKeyOk = false;
    QString publicDomain = QStringLiteral("ab2407f0.moonlightweb.top");
};

/// Assemble a request the way it goes on the wire, CRLF and all.
QByteArray wire(const char* requestLine, std::initializer_list<const char*> headers,
                const QByteArray& body = {})
{
    QByteArray raw = QByteArray(requestLine) + "\r\n";
    for (const char* h : headers)
        raw += QByteArray(h) + "\r\n";
    if (!body.isEmpty()) raw += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    raw += "\r\n";
    raw += body;
    return raw;
}

Reply serve(const QByteArray& raw, const Caller& caller = {})
{
    const HttpRequest req = HttpParser::parse(raw);

    RequestGuard::Context ctx;
    ctx.peerLocal = caller.peerLocal;
    ctx.hostSession = caller.hostSession;
    ctx.adminSession = caller.adminSession;
    ctx.adminKeyOk = caller.adminKeyOk;
    ctx.publicDomain = caller.publicDomain;

    const RequestGuard::Decision d = RequestGuard::evaluate(RequestGuard::describe(req), ctx);
    return {RequestGuard::blockStatus(d.outcome), RequestGuard::blockError(d.outcome),
            d.localPrivilege, d.adminPrivilege};
}

const QByteArray kJson = QByteArrayLiteral("{\"address\":\"192.168.1.20\"}");

} // namespace

void run_api_csrf_tests()
{
    SECTION("API CSRF — the parser feeds the guard");

    // Header names arrive in whatever case the client chose; the guard only ever
    // looks them up in lowercase. If this ever regresses, every gate below opens
    // at once, so it is asserted first and on its own.
    CHECK_EQ(serve(wire("POST /api/hosts/scan HTTP/1.1",
                        {"Host: localhost", "SEC-FETCH-SITE: cross-site"}))
                 .status,
             403);
    CHECK_EQ(serve(wire("POST /api/hosts/scan HTTP/1.1",
                        {"Host: localhost", "Sec-Fetch-Site: Cross-Site"}))
                 .status,
             403);
    // A query string must not follow the path into the route match.
    CHECK_EQ(serve(wire("GET /api/hosts?refresh=1 HTTP/1.1",
                        {"Host: localhost", "Sec-Fetch-Site: cross-site"}))
                 .status,
             403);

    SECTION("API CSRF — who initiated the request");

    // Our own frontend.
    CHECK_EQ(serve(wire("POST /api/hosts/scan HTTP/1.1",
                        {"Host: localhost", "Origin: http://localhost",
                         "Sec-Fetch-Site: same-origin", "Content-Type: application/json"},
                        kJson))
                 .status,
             0);

    // A page on another site, driving the victim's browser. This is the attack
    // from the report, verbatim.
    const Reply crossSite =
        serve(wire("POST /api/hosts/scan HTTP/1.1",
                   {"Host: localhost", "Origin: https://evil.example", "Sec-Fetch-Site: cross-site",
                    "Content-Type: application/json"},
                   kJson));
    CHECK_EQ(crossSite.status, 403);
    CHECK_EQ(crossSite.error, QString("cross_site_request_blocked"));
    CHECK(!crossSite.localPrivilege);

    // A sibling subdomain is not us either.
    CHECK_EQ(serve(wire("POST /api/hosts/scan HTTP/1.1",
                        {"Host: localhost", "Sec-Fetch-Site: same-site"}))
                 .status,
             403);

    // A sandboxed iframe, or a cross-origin redirect, sends Origin: null. An
    // attacker can produce it on purpose, so it is never ours.
    CHECK_EQ(
        serve(wire("POST /api/hosts/scan HTTP/1.1", {"Host: localhost", "Origin: null"})).status,
        403);

    // Sec-Fetch-Site beats Origin: page JavaScript cannot set the former, so a
    // forged Origin that agrees with Host must not launder the request.
    CHECK_EQ(
        serve(wire("POST /api/hosts/scan HTTP/1.1",
                   {"Host: localhost", "Origin: http://localhost", "Sec-Fetch-Site: cross-site"}))
            .status,
        403);

    // Engines predating Sec-Fetch-Site fall back to Origin vs Host.
    CHECK_EQ(serve(wire("POST /api/hosts/scan HTTP/1.1",
                        {"Host: localhost", "Origin: https://evil.example"}))
                 .status,
             403);
    CHECK_EQ(serve(wire("POST /api/hosts/scan HTTP/1.1",
                        {"Host: localhost:44729", "Origin: https://localhost:44729"}))
                 .status,
             0);

    // A user typing the address, or opening a bookmark.
    CHECK_EQ(
        serve(wire("GET /api/hosts HTTP/1.1", {"Host: localhost", "Sec-Fetch-Site: none"})).status,
        0);

    // No browser signals at all: curl, a script, a tunnel's health probe. Passing
    // these is deliberate — CSRF needs a victim's browser to borrow, and a client
    // that presents no browser signals has none to lend. Refusing them would cost
    // every non-browser caller and gain nothing, because a request a browser
    // would send always carries at least one of the two headers above.
    CHECK_EQ(serve(wire("POST /api/hosts/scan HTTP/1.1", {"Host: localhost"})).status, 0);

    SECTION("API CSRF — what a cross-origin page is able to send");

    // The second lock, and the one that covers the no-signal case: a form is the
    // only cross-origin construct that can omit Origin, and a form cannot
    // produce application/json. Everything else needs a preflight we never
    // answer (no Access-Control-Allow-* header exists in the codebase).
    CHECK_EQ(serve(wire("POST /api/hosts/manual HTTP/1.1",
                        {"Host: localhost", "Content-Type: text/plain"}, kJson))
                 .status,
             415);
    CHECK_EQ(
        serve(wire("POST /api/hosts/manual HTTP/1.1",
                   {"Host: localhost", "Content-Type: application/x-www-form-urlencoded"}, kJson))
            .status,
        415);
    CHECK_EQ(
        serve(wire("POST /api/hosts/manual HTTP/1.1",
                   {"Host: localhost", "Content-Type: multipart/form-data; boundary=x"}, kJson))
            .status,
        415);

    // `fetch(url, {method:'POST', body: new Blob([json])})` sends a body with no
    // content type, and needs no preflight.
    const Reply typeless =
        serve(wire("POST /api/hosts/manual HTTP/1.1", {"Host: localhost"}, kJson));
    CHECK_EQ(typeless.status, 415);
    CHECK_EQ(typeless.error, QString("unsupported_media_type"));

    // Legitimate shapes.
    CHECK_EQ(
        serve(wire("POST /api/hosts/manual HTTP/1.1",
                   {"Host: localhost", "Content-Type: application/json; charset=utf-8"}, kJson))
            .status,
        0);
    CHECK_EQ(serve(wire("POST /api/hosts/manual HTTP/1.1",
                        {"Host: localhost", "Content-Type: APPLICATION/JSON"}, kJson))
                 .status,
             0);
    // Nothing to parse, nothing to police.
    CHECK_EQ(serve(wire("POST /api/hosts/scan HTTP/1.1", {"Host: localhost"})).status, 0);

    SECTION("API CSRF — every method, and only the API");

    // The gate is not POST-only: DELETE removes a host, and a GET that acts is
    // reachable by a plain navigation.
    CHECK_EQ(serve(wire("DELETE /api/hosts/abc-123 HTTP/1.1",
                        {"Host: localhost", "Sec-Fetch-Site: cross-site"}))
                 .status,
             403);
    CHECK_EQ(
        serve(wire("GET /api/hosts HTTP/1.1", {"Host: localhost", "Sec-Fetch-Site: cross-site"}))
            .status,
        403);
    CHECK_EQ(serve(wire("POST /api/hosts/abc-123/pair/start HTTP/1.1",
                        {"Host: localhost", "Sec-Fetch-Site: cross-site"}))
                 .status,
             403);

    // Static files stay public: they grant nothing and change nothing, and the
    // page must keep loading from a bookmark or an embed.
    CHECK_EQ(
        serve(wire("GET /index.html HTTP/1.1", {"Host: localhost", "Sec-Fetch-Site: cross-site"}))
            .status,
        0);
    CHECK_EQ(
        serve(wire("GET /js/app.js HTTP/1.1", {"Host: localhost", "Sec-Fetch-Site: cross-site"}))
            .status,
        0);

    SECTION("API CSRF — DNS rebinding");

    // attacker.example re-pointed at 127.0.0.1 is same-origin with itself, so the
    // checks above pass and the page can even READ our answers. What it cannot do
    // is change the name the browser puts in Host — and that name is not ours, so
    // the local privilege is withheld and the admin routes refuse it.
    const Reply rebound = serve(wire("POST /api/auth/regenerate HTTP/1.1",
                                     {"Host: attacker.example", "Origin: https://attacker.example",
                                      "Sec-Fetch-Site: same-origin"}));
    CHECK_EQ(rebound.status, 0);
    CHECK(!rebound.localPrivilege);
    CHECK(!rebound.adminPrivilege);

    // The names we do answer to.
    CHECK(serve(wire("GET /api/auth/status HTTP/1.1", {"Host: localhost:44729"})).localPrivilege);
    CHECK(serve(wire("GET /api/auth/status HTTP/1.1", {"Host: 127.0.0.1"})).localPrivilege);
    CHECK(serve(wire("GET /api/auth/status HTTP/1.1", {"Host: [::1]:8080"})).localPrivilege);
    CHECK(serve(wire("GET /api/auth/status HTTP/1.1", {"Host: 192.168.1.20"})).localPrivilege);
    CHECK(serve(wire("GET /api/auth/status HTTP/1.1", {"Host: dualrtx.local"})).localPrivilege);
    // Our own domain does NOT confer it, even from loopback: a TLS-terminating
    // tunnel runs on this machine and forwards the whole internet from there,
    // under exactly that name.
    CHECK(!serve(wire("GET /api/auth/status HTTP/1.1", {"Host: ab2407f0.moonlightweb.top"}))
               .localPrivilege);
    // Someone else's tunnel is someone else's name.
    CHECK(!serve(wire("GET /api/auth/status HTTP/1.1", {"Host: xyz.trycloudflare.com"}))
               .localPrivilege);

    SECTION("API CSRF — the admin key on writes");

    // Being the host machine is not enough to write: the key is unguessable, is
    // minted fresh each run, and rides in a custom header no cross-origin
    // request can set without a preflight. It is the last barrier if every check
    // above is somehow bypassed.
    CHECK(!serve(wire("POST /api/auth/regenerate HTTP/1.1", {"Host: localhost"})).adminPrivilege);

    // The header itself is compared by HttpServer (constant-time, against the
    // key it minted); what the guard is handed is the verdict.
    Caller withKey;
    withKey.adminKeyOk = true;
    CHECK(serve(wire("POST /api/auth/regenerate HTTP/1.1",
                     {"Host: localhost", "X-MW-Admin-Key: cRk3n..."}),
                withKey)
              .adminPrivilege);

    // Reads do not need it — the admin page has to load before it can fetch it.
    CHECK(serve(wire("GET /api/auth/sessions HTTP/1.1", {"Host: localhost"})).adminPrivilege);

    // A remote caller with a session that unlocked the admin password writes
    // like the host does, key included.
    Caller remoteAdmin;
    remoteAdmin.peerLocal = false;
    remoteAdmin.adminSession = true;
    CHECK(!serve(wire("POST /api/auth/regenerate HTTP/1.1",
                      {"Host: ab2407f0.moonlightweb.top", "Sec-Fetch-Site: same-origin"}),
                 remoteAdmin)
               .adminPrivilege);
    remoteAdmin.adminKeyOk = true;
    CHECK(serve(wire("POST /api/auth/regenerate HTTP/1.1",
                     {"Host: ab2407f0.moonlightweb.top", "Sec-Fetch-Site: same-origin"}),
                remoteAdmin)
              .adminPrivilege);

    // A remote caller with no session at all gets nothing, whatever it claims.
    Caller stranger;
    stranger.peerLocal = false;
    stranger.adminKeyOk = true;
    const Reply anon =
        serve(wire("POST /api/auth/regenerate HTTP/1.1",
                   {"Host: ab2407f0.moonlightweb.top", "Sec-Fetch-Site: same-origin"}),
              stranger);
    CHECK(!anon.localPrivilege);
    CHECK(!anon.adminPrivilege);
}
