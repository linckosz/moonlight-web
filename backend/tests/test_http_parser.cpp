/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The HTTP request parser feeds routing and the auth checks, so its behaviour
 * on well-formed AND malformed input is security-relevant. These tests pin the
 * current contract (including the known duplicate-header "last wins" quirk) so
 * a future rewrite has a regression net.
 */
#include "test_framework.h"
#include "server/HttpParser.h"

static QByteArray raw(const char* s)
{
    return QByteArray(s);
}

void run_http_parser_tests()
{
    SECTION("HttpParser");

    // Basic GET: method upcased, path, headers lowercased, no body.
    {
        HttpRequest r = HttpParser::parse(
            raw("get /api/hosts HTTP/1.1\r\nHost: example.top\r\nCookie: mw_session=abc\r\n\r\n"));
        CHECK_EQ(r.method, QString("GET"));
        CHECK_EQ(r.path, QString("/api/hosts"));
        CHECK_EQ(r.headers.value("host"), QString("example.top"));
        CHECK_EQ(r.headers.value("cookie"), QString("mw_session=abc"));
        CHECK(r.body.isEmpty());
    }

    // Query string is split out of the path into queryParams.
    {
        HttpRequest r = HttpParser::parse(raw("GET /api/x?a=1&b=two HTTP/1.1\r\n\r\n"));
        CHECK_EQ(r.path, QString("/api/x"));
        CHECK_EQ(r.queryParams.value("a"), QString("1"));
        CHECK_EQ(r.queryParams.value("b"), QString("two"));
    }

    // POST body after the blank line is preserved verbatim.
    {
        HttpRequest r = HttpParser::parse(
            raw("POST /api/auth/validate HTTP/1.1\r\nContent-Type: application/json\r\n\r\n"
                "{\"pin\":\"123456\"}"));
        CHECK_EQ(r.method, QString("POST"));
        CHECK_EQ(QString::fromUtf8(r.body), QString("{\"pin\":\"123456\"}"));
    }

    // A body that itself contains CRLF must survive the header/body split.
    {
        HttpRequest r = HttpParser::parse(raw("POST /x HTTP/1.1\r\n\r\nline1\r\nline2"));
        CHECK_EQ(QString::fromUtf8(r.body), QString("line1\r\nline2"));
    }

    // Header value with a colon (e.g. a URL) keeps everything after the first colon.
    {
        HttpRequest r =
            HttpParser::parse(raw("GET / HTTP/1.1\r\nReferer: https://host/p?q=1\r\n\r\n"));
        CHECK_EQ(r.headers.value("referer"), QString("https://host/p?q=1"));
    }

    // Duplicate headers: current contract is "last wins" (documented, not ideal).
    {
        HttpRequest r =
            HttpParser::parse(raw("GET / HTTP/1.1\r\nX-H: first\r\nX-H: second\r\n\r\n"));
        CHECK_EQ(r.headers.value("x-h"), QString("second"));
    }

    // Empty path defaults to "/".
    {
        HttpRequest r = HttpParser::parse(raw("GET http://host HTTP/1.1\r\n\r\n"));
        CHECK_EQ(r.path, QString("/"));
    }

    // Degenerate inputs must not crash and must yield an empty method.
    {
        HttpRequest empty = HttpParser::parse(raw(""));
        CHECK(empty.method.isEmpty());
        HttpRequest garbage = HttpParser::parse(raw("not-a-request"));
        CHECK(garbage.method.isEmpty()); // single token → no method/path set
    }

    // ── Response splitting (CWE-113) ────────────────────────────────────────
    // QUrl::path() percent-decodes, so %0d%0a in the request target comes back
    // as a real CRLF. The HTTP→HTTPS redirect puts the path in a Location
    // header, and before this was caught the caller could append headers of
    // their own ("Set-Cookie: ..." was reproduced against a live instance).
    // Flagging it here covers every consumer, because this is the only decoder.
    {
        HttpRequest ok = HttpParser::parse(raw("GET /api/hosts HTTP/1.1\r\n\r\n"));
        CHECK(!ok.malformed);

        HttpRequest crlf =
            HttpParser::parse(raw("GET /x%0d%0aSet-Cookie:pwned=1 HTTP/1.1\r\n\r\n"));
        CHECK(crlf.malformed);

        // Either half alone is just as good for splitting a header line.
        CHECK(HttpParser::parse(raw("GET /x%0d HTTP/1.1\r\n\r\n")).malformed);
        CHECK(HttpParser::parse(raw("GET /x%0a HTTP/1.1\r\n\r\n")).malformed);

        // Other control characters have no business in a path either.
        CHECK(HttpParser::parse(raw("GET /x%00 HTTP/1.1\r\n\r\n")).malformed);
        CHECK(HttpParser::parse(raw("GET /x%7f HTTP/1.1\r\n\r\n")).malformed);

        // Query values are a different story, and the asymmetry is the point:
        // QUrl decodes the path fully but leaves control characters escaped in
        // query items, so a %0d%0a there stays four characters and can never
        // become a header break. Pinned because it is the reason the path is the
        // only vector — the parser screens query items too, which costs nothing
        // and holds the line if that decoding ever changes.
        HttpRequest q = HttpParser::parse(raw("GET /x?a=%0d%0aEvil:1 HTTP/1.1\r\n\r\n"));
        CHECK_EQ(q.path, QString("/x"));
        CHECK(!q.queryParams.value("a").contains(QLatin1Char('\r')));
        CHECK(!q.queryParams.value("a").contains(QLatin1Char('\n')));

        // Percent-encoding that is not a control character stays legitimate:
        // a space in a filename must keep working.
        CHECK(!HttpParser::parse(raw("GET /a%20b.png HTTP/1.1\r\n\r\n")).malformed);
    }
}
