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

    // ── Lines end with CR LF alone ──────────────────────────────────────────
    // The request is cut at CR LF, so a lone LF (or any other control
    // character) stays inside whatever it was sent in: the method, a header's
    // name, a header's value. None of them is one any more.
    {
        // The method: a word of capitals, 1 to 16 of them, read upper-cased.
        CHECK(HttpParser::parse(raw("GE\nT /api/health HTTP/1.1\r\n\r\n")).malformed);
        CHECK(HttpParser::parse(raw("GE\n[2026-01-01]\tX /api/health HTTP/1.1\r\n\r\n")).malformed);
        CHECK(HttpParser::parse(raw("ABCDEFGHIJKLMNOPQ /api/health HTTP/1.1\r\n\r\n")).malformed);
        CHECK(!HttpParser::parse(raw("ABCDEFGHIJKLMNOP /api/health HTTP/1.1\r\n\r\n")).malformed);
        CHECK(HttpParser::parse(raw("G-T /api/health HTTP/1.1\r\n\r\n")).malformed);
        CHECK(HttpParser::parse(raw("G1T /api/health HTTP/1.1\r\n\r\n")).malformed);
        // As before: a lower-case method is read upper-cased, and is one.
        const HttpRequest lower = HttpParser::parse(raw("get /api/health HTTP/1.1\r\n\r\n"));
        CHECK(!lower.malformed);
        CHECK_EQ(lower.method, QString("GET"));

        // The rest of the request line.
        CHECK(HttpParser::parse(raw("GET /api/health HTTP/1.1\n\r\n\r\n")).malformed);
        CHECK(HttpParser::parse(raw("GET /api/health\tHTTP/1.1\r\n\r\n")).malformed);
        const QByteArray nulLine("GET /api/health HTTP/1.1\0x\r\n\r\n", 30);
        CHECK(HttpParser::parse(nulLine).malformed);

        // A header's value: a lone LF, a CR, a NUL, a DEL; a tab stays allowed.
        CHECK(HttpParser::parse(raw("GET / HTTP/1.1\r\nOrigin: https://a\n[2026-01-01] x\r\n\r\n"))
                  .malformed);
        CHECK(HttpParser::parse(raw("GET / HTTP/1.1\r\nOrigin: https://a\rb\r\n\r\n")).malformed);
        const QByteArray nulValue("GET / HTTP/1.1\r\nX-H: a\0b\r\n\r\n", 28);
        CHECK(HttpParser::parse(nulValue).malformed);
        CHECK(HttpParser::parse(raw("GET / HTTP/1.1\r\nX-H: a\x7f"
                                    "b\r\n\r\n"))
                  .malformed);
        const HttpRequest tab =
            HttpParser::parse(raw("GET / HTTP/1.1\r\nUser-Agent: a\tb\r\n\r\n"));
        CHECK(!tab.malformed);
        CHECK_EQ(tab.headers.value("user-agent"), QString("a\tb"));

        // A header's name, tab included.
        CHECK(HttpParser::parse(raw("GET / HTTP/1.1\r\nX\nH: a\r\n\r\n")).malformed);
        CHECK(HttpParser::parse(raw("GET / HTTP/1.1\r\nX\tH: a\r\n\r\n")).malformed);

        // What some readers of a log take for a line break too: NEL (C1) and
        // the line separator, in UTF-8, in the request line, a name, a value.
        CHECK(HttpParser::parse(raw("GET /api/health\xc2\x85x HTTP/1.1\r\n\r\n")).malformed);
        CHECK(HttpParser::parse(raw("GET / HTTP/1.1\r\nX\xe2\x80\xa8H: a\r\n\r\n")).malformed);
        CHECK(HttpParser::parse(raw("GET / HTTP/1.1\r\nOrigin: a\xc2\x85"
                                    "b\r\n\r\n"))
                  .malformed);
        CHECK(HttpParser::parse(raw("GET / HTTP/1.1\r\nOrigin: a\xe2\x80\xa9"
                                    "b\r\n\r\n"))
                  .malformed);
        // Other characters past ASCII stay a value's own.
        CHECK(!HttpParser::parse(raw("GET / HTTP/1.1\r\nX-Name: caf\xc3\xa9\r\n\r\n")).malformed);

        // A request line of one word: no method, and so not served, an upgrade
        // included (HttpServer judges both on mayBeServed).
        CHECK(!HttpParser::mayBeServed(
            HttpParser::parse(raw("GARBAGE\r\nUpgrade: websocket\r\n\r\n"))));

        // The body is not a header: control characters there are its own.
        CHECK(!HttpParser::parse(raw("POST /x HTTP/1.1\r\n\r\na\nb\tc")).malformed);

        // The two checks the server also runs on requests it did not parse.
        CHECK(HttpParser::isMethod(QStringLiteral("GET")));
        CHECK(HttpParser::isMethod(QStringLiteral("OPTIONS")));
        CHECK(!HttpParser::isMethod(QString()));
        CHECK(!HttpParser::isMethod(QStringLiteral("get")));
        CHECK(!HttpParser::isMethod(QStringLiteral("GE\nT")));
        CHECK(!HttpParser::isMethod(QString(17, QLatin1Char('A'))));
        CHECK(HttpParser::isHeaderLine(QStringLiteral("origin"), QStringLiteral("https://a")));
        CHECK(HttpParser::isHeaderLine(QStringLiteral("user-agent"), QStringLiteral("a\tb")));
        CHECK(!HttpParser::isHeaderLine(QStringLiteral("origin"), QStringLiteral("a\nb")));
        CHECK(!HttpParser::isHeaderLine(QStringLiteral("x\th"), QStringLiteral("a")));
    }

    // ── A request built by its caller, as the tunnel builds its own ─────────
    // HttpServer::serveRequest serves only what mayBeServed says: a request
    // that never went through parse() is judged on the same rules.
    {
        HttpRequest built;
        built.method = QStringLiteral("GET");
        built.path = QStringLiteral("/api/health");
        built.headers.insert(QStringLiteral("origin"), QStringLiteral("https://a"));
        CHECK(HttpParser::mayBeServed(built));

        HttpRequest method = built;
        method.method = QStringLiteral("GE\n[2026-01-01] x");
        CHECK(!HttpParser::mayBeServed(method));
        method.method = QString(17, QLatin1Char('A'));
        CHECK(!HttpParser::mayBeServed(method));
        method.method = QString();
        CHECK(!HttpParser::mayBeServed(method));

        HttpRequest value = built;
        value.headers.insert(QStringLiteral("origin"), QStringLiteral("https://a\n[2026-01-01] x"));
        CHECK(!HttpParser::mayBeServed(value));
        HttpRequest name = built;
        name.headers.insert(QStringLiteral("x\nh"), QStringLiteral("a"));
        CHECK(!HttpParser::mayBeServed(name));
        HttpRequest path = built;
        path.path = QStringLiteral("/api/x\r\nSet-Cookie: a=1");
        CHECK(!HttpParser::mayBeServed(path));
        path.path = QStringLiteral("/api/x") + QChar(0x85) + QStringLiteral("y");
        CHECK(!HttpParser::mayBeServed(path));
        path.path = QStringLiteral("/api/x") + QChar(0x2028) + QStringLiteral("y");
        CHECK(!HttpParser::mayBeServed(path));
        path.path = QStringLiteral("/api/x") + QChar(0) + QStringLiteral("y");
        CHECK(!HttpParser::mayBeServed(path));
        HttpRequest flagged = built;
        flagged.malformed = true;
        CHECK(!HttpParser::mayBeServed(flagged));

        // What parse() lets through, mayBeServed serves.
        CHECK(HttpParser::mayBeServed(HttpParser::parse(
            raw("POST /api/auth/validate HTTP/1.1\r\nUser-Agent: a\tb\r\n\r\n{}"))));
    }
}
