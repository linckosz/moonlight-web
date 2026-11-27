/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Covers the CSRF / DNS-rebinding gates. The threat these encode: the server
 * grants admin access on the source IP alone, and a browser running on the host
 * lends that IP to every page it has open — so "came from 127.0.0.1" must never
 * again be enough on its own.
 */
#include "test_framework.h"
#include "server/FirstRunLink.h"
#include "server/RequestGuard.h"

#include <cstdio>
#include <vector>

namespace {

using Headers = QMap<QString, QString>;

// Headers as HttpParser produces them: keys lowercased.
Headers hdr(std::initializer_list<std::pair<const char*, const char*>> items)
{
    Headers h;
    for (const auto& kv : items)
        h[QString::fromLatin1(kv.first)] = QString::fromLatin1(kv.second);
    return h;
}

} // namespace

void run_request_guard_tests()
{
    SECTION("RequestGuard — authority normalization");

    using namespace RequestGuard;

    CHECK_EQ(normalizeAuthority("https://Example.COM"), QString("example.com"));
    CHECK_EQ(normalizeAuthority("https://example.com:443"), QString("example.com"));
    CHECK_EQ(normalizeAuthority("example.com:443"), QString("example.com"));
    CHECK_EQ(normalizeAuthority("http://localhost:80"), QString("localhost"));
    CHECK_EQ(normalizeAuthority("https://localhost:44729"), QString("localhost:44729"));
    CHECK_EQ(normalizeAuthority("[::1]:80"), QString("::1"));
    CHECK_EQ(normalizeAuthority("  "), QString());

    SECTION("RequestGuard — initiator classification");

    // Our own frontend, and a user typing the URL, are fine.
    CHECK(classifyInitiator(hdr({{"sec-fetch-site", "same-origin"}})) == Initiator::SameOrigin);
    CHECK(classifyInitiator(hdr({{"sec-fetch-site", "none"}})) == Initiator::SameOrigin);

    // A foreign page is not — including a sibling subdomain.
    CHECK(classifyInitiator(hdr({{"sec-fetch-site", "cross-site"}})) == Initiator::CrossSite);
    CHECK(classifyInitiator(hdr({{"sec-fetch-site", "same-site"}})) == Initiator::CrossSite);
    CHECK(isCrossSite(hdr({{"sec-fetch-site", "cross-site"}})));

    // Sec-Fetch-Site wins over Origin: a page cannot set it, so a forged Origin
    // that agrees with Host must not launder a cross-site request.
    CHECK(isCrossSite(hdr({{"sec-fetch-site", "cross-site"},
                           {"origin", "http://localhost"},
                           {"host", "localhost"}})));

    // No browser signals at all (curl, scripts, a tunnel's own probe): unknown,
    // and the caller lets it through — this is not a browser, so not CSRF.
    CHECK(classifyInitiator(hdr({{"host", "localhost"}})) == Initiator::Unknown);
    CHECK(!isCrossSite(hdr({{"host", "localhost"}})));

    // Legacy engines without Sec-Fetch-Site fall back to Origin vs Host.
    CHECK(classifyInitiator(hdr({{"origin", "http://localhost"}, {"host", "localhost"}})) ==
          Initiator::SameOrigin);
    CHECK(classifyInitiator(hdr({{"origin", "https://evil.example"}, {"host", "localhost"}})) ==
          Initiator::CrossSite);
    // A sandboxed iframe or a cross-origin redirect yields "null" — attacker
    // controlled, never ours.
    CHECK(classifyInitiator(hdr({{"origin", "null"}, {"host", "localhost"}})) ==
          Initiator::CrossSite);
    // Default ports must not cause a false rejection behind a TLS tunnel.
    CHECK(classifyInitiator(hdr({{"origin", "https://mw.example.com"},
                                 {"host", "mw.example.com:443"}})) == Initiator::SameOrigin);

    SECTION("RequestGuard — WebSocket origin");

    CHECK(isWebSocketOriginAllowed("http://localhost", "localhost"));
    CHECK(isWebSocketOriginAllowed("https://mw.example.com", "mw.example.com"));
    // The attack this exists for: a page that opens ws://localhost/ws to reach
    // the signaling channel (and with it the host's keyboard and mouse).
    CHECK(!isWebSocketOriginAllowed("https://evil.example", "localhost"));
    CHECK(!isWebSocketOriginAllowed("null", "localhost"));
    // Non-browser clients send no Origin and stay allowed.
    CHECK(isWebSocketOriginAllowed("", "localhost"));

    SECTION("RequestGuard — trusted host (DNS rebinding)");

    const QString domain = "mw-abc123.example.com";

    CHECK(isTrustedHost("localhost", domain));
    CHECK(isTrustedHost("localhost:44729", domain));
    CHECK(isTrustedHost("127.0.0.1", domain));
    CHECK(isTrustedHost("[::1]:443", domain));
    CHECK(isTrustedHost("192.168.1.40", domain));
    CHECK(isTrustedHost("10.1.2.3", domain));
    CHECK(isTrustedHost("172.16.0.9", domain));
    CHECK(isTrustedHost("fc00::1", domain));
    CHECK(isTrustedHost("gaming-pc.local", domain));
    CHECK(isTrustedHost(domain, domain));
    CHECK(isTrustedHost("MW-ABC123.EXAMPLE.COM", domain));
    // Non-default ports are routine here (port-parity rebind puts the public
    // domain on e.g. 44729), and the port never changes who the name means.
    CHECK(isTrustedHost(domain + ":44729", domain));
    CHECK(isTrustedHost("192.168.1.40:8443", domain));
    // HTTP/1.0 or a raw socket: no Host to rebind with.
    CHECK(isTrustedHost("", domain));

    // The rebinding signature: a name we do not own resolving to us.
    CHECK(!isTrustedHost("evil.example", domain));
    CHECK(!isTrustedHost("mw-abc123.example.com.evil.example", domain));
    CHECK(!isTrustedHost("evil.example:8080", domain));
    // A public IP literal is nobody's local address.
    CHECK(!isTrustedHost("203.0.113.7", domain));
    // With no domain configured yet, only local names qualify.
    CHECK(isTrustedHost("localhost", ""));
    CHECK(!isTrustedHost("evil.example", ""));

    SECTION("RequestGuard — machine-local host name (tunnel)");

    // Same set as above minus the public domain. Everything here names a place
    // an outsider cannot address us at.
    CHECK(isLocalHostName("localhost"));
    CHECK(isLocalHostName("localhost:44729"));
    CHECK(isLocalHostName("127.0.0.1"));
    CHECK(isLocalHostName("[::1]:443"));
    CHECK(isLocalHostName("192.168.1.40"));
    CHECK(isLocalHostName("10.1.2.3"));
    CHECK(isLocalHostName("172.16.0.9"));
    CHECK(isLocalHostName("fc00::1"));
    CHECK(isLocalHostName("169.254.7.7"));
    CHECK(isLocalHostName("gaming-pc.local"));
    CHECK(isLocalHostName(""));

    // The whole point of the split: our own domain addresses this machine (so
    // it is trusted, above) but says nothing about where the caller sits. A
    // TLS-terminating tunnel forwards the entire internet from loopback under
    // exactly this name.
    CHECK(!isLocalHostName(domain));
    CHECK(!isLocalHostName(domain + ":44729"));
    CHECK(!isLocalHostName("evil.example"));
    CHECK(!isLocalHostName("203.0.113.7"));

    SECTION("RequestGuard — a request a proxy forwarded");

    // Each header a reverse proxy, a CDN or a tunnel writes on the way.
    for (const char* name :
         {"x-forwarded-for", "x-forwarded-host", "x-forwarded-proto", "x-forwarded-server",
          "forwarded", "x-real-ip", "via", "cf-connecting-ip", "cf-ray", "cf-visitor",
          "true-client-ip", "x-client-ip", "x-cluster-client-ip", "fastly-client-ip",
          "tailscale-funnel-request", "x-original-url", "x-arr-ssl"}) {
        CHECK(forwardedByProxy(hdr({{"host", "127.0.0.1:8080"}, {name, "203.0.113.7"}})));
    }
    // What a browser or a script on this machine sends carries none of them.
    CHECK(!forwardedByProxy(hdr({})));
    CHECK(!forwardedByProxy(hdr({{"host", "localhost"},
                                 {"sec-fetch-site", "same-origin"},
                                 {"origin", "https://localhost"},
                                 {"referer", "https://localhost/admin"},
                                 {"cookie", "mw_session=abc"},
                                 {"user-agent", "curl/8.5"}})));

    // A local peer under a local name is this machine — unless a proxy stands
    // between: the peer is then the proxy, and the caller is whoever reached it.
    {
        const auto local = [&](std::initializer_list<std::pair<const char*, const char*>> more) {
            Request r;
            r.method = "GET";
            r.path = "/api/admin/settings";
            r.headers = hdr({{"host", "127.0.0.1:8080"}});
            for (const auto& kv : more)
                r.headers[QString::fromLatin1(kv.first)] = QString::fromLatin1(kv.second);
            Context c;
            c.peerLocal = true;
            return evaluate(r, c);
        };
        // The witness: nothing in between.
        const Decision direct = local({});
        CHECK(direct.outcome == Outcome::Allow);
        CHECK(direct.localPrivilege);
        CHECK(direct.hostMachine);
        CHECK(direct.adminPrivilege);

        for (const char* name : {"x-forwarded-for", "x-real-ip", "forwarded", "via",
                                 "cf-connecting-ip", "x-forwarded-proto"}) {
            const Decision proxied = local({{name, "203.0.113.7"}});
            CHECK(proxied.outcome == Outcome::Allow); // served, as any visitor is
            CHECK(!proxied.localPrivilege);
            CHECK(!proxied.hostMachine);
            CHECK(!proxied.adminPrivilege);
        }

        // The host's own browser behind such a proxy keeps what its session is
        // worth: the header takes the address away, not the session.
        {
            Request r;
            r.method = "GET";
            r.path = "/api/admin/settings";
            r.headers = hdr({{"host", "127.0.0.1:8080"}, {"x-forwarded-for", "192.168.1.9"}});
            Context c;
            c.peerLocal = true;
            c.hostSession = true;
            const Decision d = evaluate(r, c);
            CHECK(!d.localPrivilege);
            CHECK(d.hostMachine);
            CHECK(d.adminPrivilege);
        }
    }

    SECTION("RequestGuard — this machine on loopback, and nothing wider");

    // The one plain-HTTP page the host is not sent to HTTPS for (the first-run
    // wizard): a loopback peer, under a loopback name, with no proxy between.
    CHECK(loopbackOnly("127.0.0.1", "localhost:8080", hdr({})));
    CHECK(loopbackOnly("127.0.0.1", "LOCALHOST:8080", hdr({})));
    CHECK(loopbackOnly("127.0.0.1", "127.0.0.1:8080", hdr({})));
    CHECK(loopbackOnly("127.0.0.5", "localhost", hdr({})));
    CHECK(loopbackOnly("::1", "[::1]:8080", hdr({})));
    CHECK(loopbackOnly("::ffff:127.0.0.1", "localhost:8080", hdr({})));
    // A LAN caller that writes "localhost" in Host: sent to HTTPS.
    CHECK(!loopbackOnly("192.168.1.9", "localhost:8080", hdr({})));
    CHECK(!loopbackOnly("::ffff:192.168.1.9", "localhost:8080", hdr({})));
    CHECK(!loopbackOnly("fe80::1", "[::1]:8080", hdr({})));
    // This machine under a wider name: the local privilege has it, this does not.
    CHECK(!loopbackOnly("127.0.0.1", "192.168.1.40:8080", hdr({})));
    CHECK(!loopbackOnly("127.0.0.1", "gaming-pc.local:8080", hdr({})));
    CHECK(!loopbackOnly("127.0.0.1", domain, hdr({})));
    CHECK(!loopbackOnly("127.0.0.1", "evil.example", hdr({})));
    CHECK(!loopbackOnly("127.0.0.1", "localhost.evil.example", hdr({})));
    CHECK(!loopbackOnly("127.0.0.1", "", hdr({})));
    CHECK(!loopbackOnly("", "localhost", hdr({})));
    CHECK(!loopbackOnly("not-an-address", "localhost", hdr({})));
    // A local reverse proxy relaying the LAN under "localhost": sent to HTTPS.
    for (const char* name : {"x-forwarded-for", "x-forwarded-host", "x-real-ip", "forwarded", "via",
                             "cf-connecting-ip"}) {
        CHECK(!loopbackOnly("127.0.0.1", "localhost:8080",
                            hdr({{"host", "localhost:8080"}, {name, "192.168.1.9"}})));
    }
    // Never wider than the local privilege: whatever it holds for is this
    // machine to evaluate() too.
    {
        Request r;
        r.method = "GET";
        r.path = "/api/setup/status";
        r.headers = hdr({{"host", "localhost:8080"}});
        CHECK(loopbackOnly("127.0.0.1", r.headers.value("host"), r.headers));
        Context c;
        c.peerLocal = true;
        CHECK(evaluate(r, c).localPrivilege);
        // An arrival through the rendezvous tunnel is never a local peer
        // (HttpServer::serveRequest: Arrival::Socket only), so a Host of
        // "localhost" there is worth nothing: no local privilege.
        Context tunnel;
        tunnel.peerLocal = false;
        CHECK(!evaluate(r, tunnel).localPrivilege);
        CHECK(!evaluate(r, tunnel).hostMachine);
    }

    SECTION("RequestGuard — a local reverse proxy that took the browser's HTTPS");

    {
        const QSet<QString> none;
        // Served on the plain port: the proxy reports HTTPS for every hop.
        CHECK(proxiedHttpsOnLoopback("127.0.0.1", hdr({{"x-forwarded-proto", "https"}}), none));
        CHECK(proxiedHttpsOnLoopback("127.0.0.1", hdr({{"x-forwarded-proto", "HTTPS"}}), none));
        CHECK(proxiedHttpsOnLoopback("::1", hdr({{"forwarded", "proto=https"}}), none));
        CHECK(proxiedHttpsOnLoopback("::ffff:127.0.0.1",
                                     hdr({{"forwarded", "for=192.168.1.9;proto=HTTPS"}}), none));
        CHECK(proxiedHttpsOnLoopback(
            "127.0.0.1",
            hdr({{"forwarded", "for=192.168.1.9;proto=\"https\", for=10.0.0.2;proto=https"}}),
            none));
        // A proxy on this machine that keeps the browser's LAN name in Host,
        // with the browser on HTTPS at the proxy: served, not sent away.
        CHECK(proxiedHttpsOnLoopback("127.0.0.1",
                                     hdr({{"host", "192.168.1.40:8080"},
                                          {"x-forwarded-for", "192.168.1.9"},
                                          {"x-forwarded-proto", "https"}}),
                                     none));
        // Sent to HTTPS: plain HTTP at the proxy, mixed, or unknown.
        CHECK(!proxiedHttpsOnLoopback("127.0.0.1", hdr({{"x-forwarded-proto", "http"}}), none));
        CHECK(!proxiedHttpsOnLoopback("127.0.0.1", hdr({{"x-forwarded-proto", "https, http"}}),
                                      none));
        CHECK(!proxiedHttpsOnLoopback("127.0.0.1", hdr({{"x-forwarded-proto", "https,"}}), none));
        CHECK(!proxiedHttpsOnLoopback("127.0.0.1", hdr({{"x-forwarded-proto", ""}}), none));
        CHECK(!proxiedHttpsOnLoopback(
            "127.0.0.1", hdr({{"forwarded", "for=1.2.3.4;proto=https, for=5.6.7.8;proto=http"}}),
            none));
        CHECK(!proxiedHttpsOnLoopback("127.0.0.1", hdr({{"forwarded", "for=192.168.1.9"}}), none));
        // A comma inside quotes splits the element: its half without proto=
        // is a no, whatever the quotes held.
        CHECK(!proxiedHttpsOnLoopback(
            "127.0.0.1", hdr({{"forwarded", "for=\"a,proto=https\";proto=http"}}), none));
        CHECK(!proxiedHttpsOnLoopback("127.0.0.1", hdr({{"forwarded", "proto=https;proto=https"}}),
                                      none));
        CHECK(!proxiedHttpsOnLoopback(
            "127.0.0.1", hdr({{"x-forwarded-proto", "https"}, {"forwarded", "proto=http"}}), none));
        // The peer must be this machine on loopback, not merely local.
        CHECK(!proxiedHttpsOnLoopback("192.168.1.9", hdr({{"x-forwarded-proto", "https"}}), none));
        CHECK(!proxiedHttpsOnLoopback("::ffff:192.168.1.9", hdr({{"x-forwarded-proto", "https"}}),
                                      none));
        CHECK(!proxiedHttpsOnLoopback("", hdr({{"x-forwarded-proto", "https"}}), none));
        // No protocol reported at all: the wizard's bare loopback is
        // loopbackOnly's, and a proxy that says nothing is sent to HTTPS.
        CHECK(!proxiedHttpsOnLoopback("127.0.0.1", hdr({}), none));
        CHECK(
            !proxiedHttpsOnLoopback("127.0.0.1", hdr({{"x-forwarded-for", "192.168.1.9"}}), none));
        // A header given twice: its other values are unknown.
        CHECK(!proxiedHttpsOnLoopback("127.0.0.1", hdr({{"x-forwarded-proto", "https"}}),
                                      QSet<QString>{"x-forwarded-proto"}));
        CHECK(!proxiedHttpsOnLoopback("127.0.0.1", hdr({{"forwarded", "proto=https"}}),
                                      QSet<QString>{"forwarded"}));

        // Serving it grants nothing: an admin route from that proxy, with no
        // session and no PIN, has neither the local privilege nor admin.
        Request r;
        r.method = "GET";
        r.path = "/api/admin/settings";
        r.headers = hdr({{"host", "192.168.1.40:8080"},
                         {"x-forwarded-for", "192.168.1.9"},
                         {"x-forwarded-proto", "https"}});
        CHECK(proxiedHttpsOnLoopback("127.0.0.1", r.headers, none));
        Context c;
        c.peerLocal = true;
        const Decision d = evaluate(r, c);
        CHECK(!d.localPrivilege);
        CHECK(!d.hostMachine);
        CHECK(!d.adminPrivilege);
        // The same under a loopback Host, nginx's default.
        r.headers = hdr({{"host", "127.0.0.1:8080"},
                         {"x-forwarded-for", "192.168.1.9"},
                         {"x-forwarded-proto", "https"}});
        CHECK(proxiedHttpsOnLoopback("127.0.0.1", r.headers, none));
        CHECK(!loopbackOnly("127.0.0.1", r.headers.value("host"), r.headers));
        const Decision d2 = evaluate(r, c);
        CHECK(!d2.localPrivilege);
        CHECK(!d2.hostMachine);
        CHECK(!d2.adminPrivilege);
    }

    SECTION("RequestGuard — a reverse proxy on the LAN that took the browser's HTTPS");

    {
        const QSet<QString> none;
        const QString box = "192.168.1.1";
        const Headers https = hdr({{"x-forwarded-proto", "https"}});
        // Served: a private IPv4 peer, every protocol https.
        CHECK(proxiedHttpsFromLan("192.168.1.20", https, none, box));
        CHECK(proxiedHttpsFromLan("10.0.0.5", hdr({{"forwarded", "proto=https"}}), none, box));
        CHECK(proxiedHttpsFromLan("172.20.1.1", hdr({{"forwarded", "for=192.168.1.9;proto=HTTPS"}}),
                                  none, box));
        CHECK(proxiedHttpsFromLan("::ffff:192.168.1.20", https, none, box));
        CHECK(proxiedHttpsFromLan("169.254.3.4", https, none, box));
        // The router unknown: no exclusion.
        CHECK(proxiedHttpsFromLan("192.168.1.1", https, none, QString()));
        // Sent to HTTPS: the router, or a peer that is not of the LAN.
        CHECK(!proxiedHttpsFromLan("192.168.1.1", https, none, box));
        CHECK(!proxiedHttpsFromLan("::ffff:192.168.1.1", https, none, box));
        CHECK(!proxiedHttpsFromLan("100.64.1.1", https, none, box));
        CHECK(!proxiedHttpsFromLan("100.127.255.254", https, none, box));
        CHECK(!proxiedHttpsFromLan("8.8.8.8", https, none, box));
        CHECK(!proxiedHttpsFromLan("172.32.0.1", https, none, box));
        CHECK(!proxiedHttpsFromLan("fd00::1", https, none, box));
        CHECK(!proxiedHttpsFromLan("fe80::1", https, none, box));
        CHECK(!proxiedHttpsFromLan("127.0.0.1", https, none, box));
        CHECK(!proxiedHttpsFromLan("", https, none, box));
        // Sent to HTTPS: plain, mixed, repeated or unknown protocols.
        CHECK(
            !proxiedHttpsFromLan("192.168.1.20", hdr({{"x-forwarded-proto", "http"}}), none, box));
        CHECK(!proxiedHttpsFromLan("192.168.1.20", hdr({{"x-forwarded-proto", "https, http"}}),
                                   none, box));
        CHECK(!proxiedHttpsFromLan("192.168.1.20", https, QSet<QString>{"x-forwarded-proto"}, box));
        CHECK(!proxiedHttpsFromLan("192.168.1.20", hdr({{"forwarded", "for=192.168.1.9"}}), none,
                                   box));
        CHECK(!proxiedHttpsFromLan(
            "192.168.1.20", hdr({{"forwarded", "for=\"a,proto=https\";proto=http"}}), none, box));
        CHECK(!proxiedHttpsFromLan("192.168.1.20", hdr({{"x-forwarded-ssl", "on"}}), none, box));
        CHECK(!proxiedHttpsFromLan("192.168.1.20", hdr({{"front-end-https", "on"}}), none, box));
        CHECK(!proxiedHttpsFromLan("192.168.1.20", hdr({{"x-forwarded-scheme", "https"}}), none,
                                   box));
        CHECK(!proxiedHttpsFromLan("192.168.1.20", hdr({{"x-forwarded-for", "192.168.1.9"}}), none,
                                   box));

        // Serving it grants nothing: an admin route from that proxy, with no
        // session and no PIN, has neither the local privilege nor admin.
        Request r;
        r.method = "GET";
        r.path = "/api/admin/settings";
        r.headers = hdr({{"host", "192.168.1.40:8080"},
                         {"x-forwarded-for", "192.168.1.9"},
                         {"x-forwarded-proto", "https"}});
        CHECK(proxiedHttpsFromLan("192.168.1.20", r.headers, none, box));
        Context lan;
        lan.peerLocal = false;
        const Decision d = evaluate(r, lan);
        CHECK(!d.localPrivilege);
        CHECK(!d.hostMachine);
        CHECK(!d.adminPrivilege);
    }

    SECTION("RequestGuard — what a request on the plain-HTTP port gets, in order");

    {
        const QSet<QString> none;
        const QString box = "192.168.1.1";
        const Headers lanProxy = hdr({{"host", "192.168.1.40:8080"},
                                      {"x-forwarded-for", "192.168.1.9"},
                                      {"x-forwarded-proto", "https"}});
        using P = PlainArrival;
        // A listed TV keeps its own path first, even with a proxy's headers.
        CHECK(plainArrival(true, false, "192.168.1.20", "192.168.1.40:8080", lanProxy, none, box) ==
              P::LanHttp);
        CHECK(plainArrival(true, true, "127.0.0.1", "example.com", lanProxy, none, box) ==
              P::LanHttp);
        // Then the four that are served.
        CHECK(plainArrival(false, true, "127.0.0.1", "stream.example.com", hdr({}), none, box) ==
              P::Served);
        CHECK(plainArrival(false, false, "127.0.0.1", "localhost:8080", hdr({}), none, box) ==
              P::Served);
        CHECK(plainArrival(false, false, "127.0.0.1", "192.168.1.40:8080", lanProxy, none, box) ==
              P::Served);
        CHECK(plainArrival(false, false, "192.168.1.20", "192.168.1.40:8080", lanProxy, none,
                           box) == P::Served);
        // Anything else is sent to HTTPS.
        CHECK(plainArrival(false, false, "192.168.1.20", "192.168.1.40:8080", hdr({}), none, box) ==
              P::Redirected);
        CHECK(plainArrival(false, false, "192.168.1.1", "192.168.1.40:8080", lanProxy, none, box) ==
              P::Redirected);
        CHECK(plainArrival(false, false, "100.64.1.1", "192.168.1.40:8080", lanProxy, none, box) ==
              P::Redirected);
        CHECK(plainArrival(false, false, "192.168.1.20", "localhost:8080", hdr({}), none, box) ==
              P::Redirected);
    }

    SECTION("RequestGuard — the line that tells a proxy what to add");

    {
        const QSet<QString> none;
        // Wanted: a proxy here or on the LAN that does not report HTTPS.
        CHECK(proxyHintWanted("127.0.0.1", hdr({{"x-forwarded-for", "192.168.1.9"}}), none));
        CHECK(proxyHintWanted("192.168.1.20", hdr({{"via", "1.1 proxy"}}), none));
        CHECK(proxyHintWanted("192.168.1.20", hdr({{"forwarded", "for=192.168.1.9"}}), none));
        CHECK(proxyHintWanted("192.168.1.20", hdr({{"x-forwarded-ssl", "on"}}), none));
        CHECK(proxyHintWanted("192.168.1.20", hdr({{"x-forwarded-proto", "https"}}),
                              QSet<QString>{"x-forwarded-proto"}));
        // Not wanted: no proxy, one that reports HTTPS, or not ours to help.
        CHECK(!proxyHintWanted("192.168.1.20", hdr({{"host", "192.168.1.40"}}), none));
        CHECK(!proxyHintWanted("192.168.1.20", hdr({{"x-forwarded-proto", "https"}}), none));
        CHECK(!proxyHintWanted("8.8.8.8", hdr({{"x-forwarded-for", "192.168.1.9"}}), none));
        CHECK(!proxyHintWanted("127.0.0.1", hdr({{"host", "localhost"}}), none));

        // Header names, never their values.
        const QString text = proxyHintText("192.168.1.20",
                                           hdr({{"x-forwarded-for", "203.0.113.77"},
                                                {"via", "1.1 secret-proxy-name"},
                                                {"x-forwarded-proto", "http-secret-value"}}),
                                           none);
        CHECK(text.contains("192.168.1.20"));
        CHECK(text.contains("x-forwarded-for"));
        CHECK(text.contains("via"));
        CHECK(text.contains("X-Forwarded-Proto"));
        CHECK(!text.contains("203.0.113.77"));
        CHECK(!text.contains("secret-proxy-name"));
        CHECK(!text.contains("http-secret-value"));
        const QString twice = proxyHintText("192.168.1.20", hdr({{"x-forwarded-proto", "https"}}),
                                            QSet<QString>{"x-forwarded-proto"});
        CHECK(twice.contains("given more than once"));

        // Once per peer per window, 20 lines per window in all, then a single
        // "suppressed", then nothing until the window ends.
        using V = ProxyHintLimiter::Verdict;
        const qint64 w = ProxyHintLimiter::kWindowMs;
        ProxyHintLimiter limiter;
        CHECK(limiter.take("192.168.1.20", 0) == V::Write);
        CHECK(limiter.take("192.168.1.20", 1000) == V::Silent);
        CHECK(limiter.take("192.168.1.21", 1000) == V::Write);
        for (int i = 2; i < ProxyHintLimiter::kGlobalMax; ++i)
            CHECK(limiter.take(QString("10.0.0.%1").arg(i), 2000) == V::Write);
        CHECK(limiter.take("10.0.1.1", 3000) == V::Suppressed);
        CHECK(limiter.take("10.0.1.2", 3000) == V::Silent);
        CHECK(limiter.take("10.0.1.3", w - 1) == V::Silent);
        // A new window: the global count starts again; a peer heard in the
        // last window is heard again once its own window has passed.
        CHECK(limiter.take("10.0.1.4", w) == V::Write);
        CHECK(limiter.take("192.168.1.20", w) == V::Write);
        CHECK(limiter.take("192.168.1.21", w + 999) == V::Silent);
        CHECK(limiter.take("192.168.1.21", w + 1000) == V::Write);

        // The 32 most recent peers are kept: the oldest is forgotten even
        // while its own window still runs. One peer opens the global window at
        // 10 ms, 19 more come just before it ends, then 14 in the next one:
        // 34 heard, the two oldest forgotten, the second while still recent.
        ProxyHintLimiter lru;
        CHECK(lru.take("10.9.9.9", 10) == V::Write);
        for (int i = 0; i < 19; ++i)
            CHECK(lru.take(QString("10.1.0.%1").arg(i), w - 1) == V::Write);
        for (int i = 0; i < 14; ++i)
            CHECK(lru.take(QString("10.2.0.%1").arg(i), w + 10) == V::Write);
        CHECK(lru.take("10.1.0.1", w + 20) == V::Silent);
        CHECK(lru.take("10.1.0.0", w + 20) == V::Write);
    }

    SECTION("First-run wizard link: plain HTTP loopback, no key");

    // The port actually listened on; no key in it; served by loopbackOnly.
    CHECK_EQ(FirstRunLink::wizardUrl(8080), QString("http://localhost:8080/setup"));
    CHECK_EQ(FirstRunLink::wizardUrl(47990), QString("http://localhost:47990/setup"));
    CHECK_EQ(FirstRunLink::wizardUrl(80), QString("http://localhost/setup"));
    CHECK(!FirstRunLink::wizardUrl(8080).contains(QLatin1Char('#')));
    CHECK(loopbackOnly("127.0.0.1", "localhost:8080", hdr({{"host", "localhost:8080"}})));
    // No plain listener: empty, and the opener keeps its HTTPS link.
    CHECK(FirstRunLink::wizardUrl(0).isEmpty());

    SECTION("RequestGuard — body content type");

    CHECK(isBodyContentTypeAllowed("application/json", 12));
    CHECK(isBodyContentTypeAllowed("application/json; charset=utf-8", 12));
    CHECK(isBodyContentTypeAllowed("APPLICATION/JSON", 12));
    // An empty body carries nothing to parse.
    CHECK(isBodyContentTypeAllowed("", 0));
    CHECK(isBodyContentTypeAllowed("text/plain", 0));

    // The CORS-safelisted types, which is exactly what a cross-origin fetch can
    // send without a preflight — and what the parser used to accept blindly.
    CHECK(!isBodyContentTypeAllowed("text/plain;charset=UTF-8", 12));
    CHECK(!isBodyContentTypeAllowed("application/x-www-form-urlencoded", 12));
    CHECK(!isBodyContentTypeAllowed("multipart/form-data; boundary=x", 12));
    // `fetch(url, {method:'POST', body: new Blob([json])})` sends no type at all.
    CHECK(!isBodyContentTypeAllowed("", 12));

    SECTION("RequestGuard — access decision");

    // Shorthands for the two callers that matter: our own admin page, and a
    // malicious page open in the same browser on the host machine.
    const auto frontendPost = [&](const char* path, bool withKey) {
        Request r;
        r.method = "POST";
        r.path = path;
        r.headers = hdr({{"sec-fetch-site", "same-origin"},
                         {"origin", "http://localhost"},
                         {"host", "localhost"},
                         {"content-type", "application/json"}});
        r.bodySize = 2;
        Context c;
        c.peerLocal = true;
        c.adminKeyOk = withKey;
        return evaluate(r, c);
    };

    // The legitimate admin page: same-origin, JSON, carrying the admin key.
    {
        const Decision d = frontendPost("/api/admin/settings", true);
        CHECK(d.outcome == Outcome::Allow);
        CHECK(d.localPrivilege);
        CHECK(d.adminPrivilege);
    }

    // The attack from the report: evil.example makes the host's browser POST to
    // localhost. It reaches us from 127.0.0.1 like the real UI would.
    {
        Request r;
        r.method = "POST";
        r.path = "/api/setup/apply";
        r.headers = hdr({{"sec-fetch-site", "cross-site"},
                         {"origin", "https://evil.example"},
                         {"host", "localhost"},
                         {"content-type", "application/json"}});
        r.bodySize = 64;
        Context c;
        c.peerLocal = true; // ← the whole problem: the IP looks local
        const Decision d = evaluate(r, c);
        CHECK(d.outcome == Outcome::BlockCrossSite);
        CHECK(!d.localPrivilege);
        CHECK(!d.adminPrivilege);
    }

    // Same attack without Sec-Fetch-Site (older engine): Origin still betrays it.
    {
        Request r;
        r.method = "POST";
        r.path = "/api/setup/apply";
        r.headers = hdr({{"origin", "https://evil.example"},
                         {"host", "localhost"},
                         {"content-type", "application/json"}});
        r.bodySize = 64;
        Context c;
        c.peerLocal = true;
        CHECK(evaluate(r, c).outcome == Outcome::BlockCrossSite);
    }

    // Same attack dodging the preflight with a CORS-safelisted content type —
    // the parser never looked at Content-Type, so this used to be enough.
    {
        Request r;
        r.method = "POST";
        r.path = "/api/setup/apply";
        r.headers = hdr({{"host", "localhost"}, {"content-type", "text/plain;charset=UTF-8"}});
        r.bodySize = 64;
        Context c;
        c.peerLocal = true;
        CHECK(evaluate(r, c).outcome == Outcome::BlockContentType);
    }

    // DNS rebinding: the attacker's page IS same-origin with us (so the checks
    // above all pass, and it can read our responses), but Host is not ours.
    {
        Request r;
        r.method = "GET";
        r.path = "/api/auth/status"; // hands the PIN to local callers
        r.headers = hdr({{"sec-fetch-site", "same-origin"},
                         {"origin", "http://evil.example"},
                         {"host", "evil.example"}});
        Context c;
        c.peerLocal = true;
        const Decision d = evaluate(r, c);
        CHECK(d.outcome == Outcome::Allow); // not CSRF — it really is same-origin
        CHECK(d.hostUntrusted);
        CHECK(!d.localPrivilege); // …but it gets nothing
        CHECK(!d.adminPrivilege);
    }

    // An admin write from the host without the key (a stale tab, or a local
    // script): refused the admin routes, still exempt from the PIN.
    {
        const Decision d = frontendPost("/api/admin/settings", false);
        CHECK(d.outcome == Outcome::Allow);
        CHECK(d.localPrivilege);
        CHECK(!d.adminPrivilege);
    }

    // Reads keep working on the address alone — the admin page must be able to
    // load before it can hold a key.
    {
        Request r;
        r.method = "GET";
        r.path = "/api/admin/settings";
        r.headers = hdr({{"sec-fetch-site", "same-origin"}, {"host", "localhost"}});
        Context c;
        c.peerLocal = true;
        const Decision d = evaluate(r, c);
        CHECK(d.localPrivilege);
        CHECK(d.adminPrivilege);
    }

    // curl on the host: no browser headers at all, and no body. Unchanged
    // behaviour — it is not a browser, so it cannot be a CSRF vector.
    {
        Request r;
        r.method = "GET";
        r.path = "/api/hosts";
        r.headers = hdr({{"host", "127.0.0.1:48000"}});
        Context c;
        c.peerLocal = true;
        const Decision d = evaluate(r, c);
        CHECK(d.outcome == Outcome::Allow);
        CHECK(d.localPrivilege);
    }

    // A remote browser with a valid session: no local privilege, and its admin
    // writes stay refused (no host-key session, no key).
    {
        Request r;
        r.method = "POST";
        r.path = "/api/admin/settings";
        r.headers = hdr({{"sec-fetch-site", "same-origin"},
                         {"host", "mw-abc123.example.com"},
                         {"content-type", "application/json"}});
        r.bodySize = 12;
        Context c;
        c.peerLocal = false;
        c.publicDomain = "mw-abc123.example.com";
        const Decision d = evaluate(r, c);
        CHECK(d.outcome == Outcome::Allow);
        CHECK(!d.localPrivilege);
        CHECK(!d.adminPrivilege);
    }

    // The host machine reaching us over the public domain (host-key session):
    // admin access with the key, as the desktop shortcut flow provides.
    {
        Request r;
        r.method = "POST";
        r.path = "/api/admin/settings";
        r.headers = hdr({{"sec-fetch-site", "same-origin"},
                         {"host", "mw-abc123.example.com"},
                         {"content-type", "application/json"}});
        r.bodySize = 12;
        Context c;
        c.peerLocal = false;
        c.hostSession = true;
        c.adminKeyOk = true;
        c.publicDomain = "mw-abc123.example.com";
        const Decision d = evaluate(r, c);
        CHECK(d.adminPrivilege);
        CHECK(!d.localPrivilege); // still needs its session for non-admin routes
    }

    // A LAN machine that unlocked with the remote admin password: same admin
    // access as the host, but it is NOT the host machine — the distinction
    // gates the setup wizard and the host key.
    {
        Request r;
        r.method = "POST";
        r.path = "/api/admin/settings";
        r.headers = hdr({{"sec-fetch-site", "same-origin"},
                         {"host", "10.0.0.9"},
                         {"content-type", "application/json"}});
        r.bodySize = 12;
        Context c;
        c.peerLocal = false;
        c.adminSession = true;
        c.adminKeyOk = true;
        const Decision d = evaluate(r, c);
        CHECK(d.adminPrivilege);
        CHECK(!d.hostMachine);
        CHECK(!d.localPrivilege);
    }

    // Same session without the admin key: reads pass (the admin page has to
    // load), writes do not. The unlock does not exempt anyone from CSRF.
    {
        Request r;
        r.method = "POST";
        r.path = "/api/admin/settings";
        r.headers = hdr({{"sec-fetch-site", "same-origin"},
                         {"host", "10.0.0.9"},
                         {"content-type", "application/json"}});
        r.bodySize = 12;
        Context c;
        c.peerLocal = false;
        c.adminSession = true;
        c.adminKeyOk = false;
        CHECK(!evaluate(r, c).adminPrivilege);

        r.method = "GET";
        r.bodySize = 0;
        CHECK(evaluate(r, c).adminPrivilege);
    }

    // Loopback peer under our own name is the host machine; the same peer under
    // a stranger's name (a third-party tunnel, or DNS rebinding) is not, and
    // hostTrusted says so — that flag is what keeps the password unlock off the
    // internet.
    {
        Request r;
        r.method = "GET";
        r.path = "/api/auth/status";
        r.headers = hdr({{"sec-fetch-site", "same-origin"}, {"host", "localhost"}});
        Context c;
        c.peerLocal = true;
        c.publicDomain = "mw-abc123.example.com";
        const Decision d = evaluate(r, c);
        CHECK(d.hostTrusted);
        CHECK(d.hostMachine);

        r.headers = hdr({{"sec-fetch-site", "same-origin"}, {"host", "random.trycloudflare.com"}});
        const Decision d2 = evaluate(r, c);
        CHECK(!d2.hostTrusted);
        CHECK(!d2.hostMachine);
        CHECK(d2.hostUntrusted);
    }

    // A TLS-terminating tunnel (cloudflared, nport) runs ON this machine and
    // forwards every visitor on earth from loopback, under our own domain. Both
    // halves of "local peer + trusted Host" are therefore satisfied by total
    // strangers, which is why the local privilege now asks the stricter
    // question: could this NAME only have been reached from here?
    {
        Request r;
        r.method = "GET";
        r.path = "/api/hosts";
        r.headers = hdr({{"sec-fetch-site", "same-origin"}, {"host", "mw-abc123.example.com"}});
        Context c;
        c.peerLocal = true; // the tunnel process, not the visitor
        c.publicDomain = "mw-abc123.example.com";
        const Decision d = evaluate(r, c);
        CHECK(d.outcome == Outcome::Allow); // it is a normal remote request…
        CHECK(d.hostTrusted);               // …under a name we really do own…
        CHECK(!d.hostUntrusted);
        CHECK(!d.localPrivilege); // …which proves nothing about where it came from
        CHECK(!d.hostMachine);
        CHECK(!d.adminPrivilege);

        // The host machine reaching itself through that same domain still
        // passes: it redeems the host key on the parity redirect.
        c.hostSession = true;
        CHECK(evaluate(r, c).hostMachine);
    }

    // Static files are never gated: a cross-site load of the app shell (or a
    // bookmarked page) must keep working.
    {
        Request r;
        r.method = "GET";
        r.path = "/index.html";
        r.headers = hdr({{"sec-fetch-site", "cross-site"}, {"host", "localhost"}});
        Context c;
        c.peerLocal = true;
        CHECK(evaluate(r, c).outcome == Outcome::Allow);
    }

    SECTION("RequestGuard — admin token reply");

    {
        Decision hostMachine;
        hostMachine.hostMachine = true;
        Decision remote; // nothing granted

        Context plain;
        Context admin;
        admin.adminSession = true;

        // The host's own page, and a remote session that unlocked admin with the
        // password, both get the key.
        CHECK(adminTokenReply(hostMachine, plain, true) == AdminTokenReply::Grant);
        CHECK(adminTokenReply(remote, admin, true) == AdminTokenReply::Grant);

        // A valid remote session without admin rights gets an empty answer. The
        // regression this guards: replying Deny here fed ConnectionGuard on every
        // page load, and after AUTHFAIL_MAX loads the user was banned from their
        // own server for 10 minutes — with a perfectly valid session.
        CHECK(adminTokenReply(remote, plain, true) == AdminTokenReply::Empty);

        // No session at all is the only case that counts as fishing.
        CHECK(adminTokenReply(remote, plain, false) == AdminTokenReply::Deny);

        // --noadmin: nobody gets the key, not even the host or an unlocked
        // session — and the host, which has no session, is not fishing.
        Context locked;
        locked.adminLocked = true;
        locked.adminSession = true;
        CHECK(adminTokenReply(hostMachine, locked, false) == AdminTokenReply::Empty);
        CHECK(adminTokenReply(remote, locked, true) == AdminTokenReply::Empty);
        CHECK(adminTokenReply(remote, locked, false) == AdminTokenReply::Deny);
    }

    SECTION("RequestGuard — booth mode (--noadmin)");

    {
        // The host's own admin page, key and all, is no admin any more — but
        // it is still the host machine, which is what lets it stream.
        Request r;
        r.method = "POST";
        r.path = "/api/admin/settings";
        r.headers = hdr({{"sec-fetch-site", "same-origin"},
                         {"origin", "http://localhost"},
                         {"host", "localhost"},
                         {"content-type", "application/json"}});
        r.bodySize = 2;
        Context c;
        c.peerLocal = true;
        c.adminKeyOk = true;
        c.adminLocked = true;
        const Decision d = evaluate(r, c);
        CHECK(d.outcome == Outcome::Allow);
        CHECK(d.hostMachine);
        CHECK(!d.adminPrivilege);
        r.method = "GET";
        CHECK(!evaluate(r, c).adminPrivilege);
    }

    // What the booth changes, refused.
    CHECK(lockedByNoAdmin("POST", "/api/hosts/manual"));
    // The tray's Disconnect (issue #42), on every path.
    CHECK(lockedByNoAdmin("POST", "/api/server/stream-disconnect"));
    CHECK(!lockedByNoAdmin("GET", "/api/server/stream-activity"));
    CHECK(lockedByNoAdmin("POST", "/api/hosts/scan"));
    CHECK(lockedByNoAdmin("DELETE", "/api/hosts/uuid-1"));
    CHECK(lockedByNoAdmin("POST", "/api/hosts/uuid-1/name"));
    CHECK(lockedByNoAdmin("POST", "/api/hosts/uuid-1/pair/start"));
    CHECK(lockedByNoAdmin("POST", "/api/hosts/uuid-1/pair"));
    CHECK(lockedByNoAdmin("POST", "/api/hosts/uuid-1/restart"));
    CHECK(lockedByNoAdmin("POST", "/api/hosts/uuid-1/backend"));
    CHECK(lockedByNoAdmin("DELETE", "/api/hosts/uuid-1/backend"));
    CHECK(lockedByNoAdmin("POST", "/api/hosts/uuid-1/seats"));
    CHECK(lockedByNoAdmin("DELETE", "/api/hosts/uuid-1/seats/2/owner"));
    CHECK(lockedByNoAdmin("POST", "/api/hosts/uuid-1/some-future-route"));
    CHECK(lockedByNoAdmin("POST", "/api/update/start"));
    CHECK(lockedByNoAdmin("POST", "/api/system/restart"));
    CHECK(lockedByNoAdmin("POST", "/api/system/gamepad-driver/install"));
    // No new device, no new guest.
    CHECK(lockedByNoAdmin("POST", "/api/auth/validate"));
    CHECK(lockedByNoAdmin("POST", "/api/auth/admin-unlock"));
    CHECK(lockedByNoAdmin("POST", "/api/auth/regenerate"));
    CHECK(lockedByNoAdmin("POST", "/api/auth/pin/start"));
    CHECK(lockedByNoAdmin("POST", "/api/auth/pin/finish"));
    CHECK(lockedByNoAdmin("POST", "/api/share/slots/2/activate"));
    CHECK(lockedByNoAdmin("POST", "/api/share/player/pin"));
    // Nor by the QR code a device shows: its request, its claim, the answer.
    CHECK(lockedByNoAdmin("POST", "/api/auth/pair-request"));
    CHECK(lockedByNoAdmin("POST", "/api/auth/pair-request/AbCdEfGhIjKlMnOp/claim"));
    CHECK(lockedByNoAdmin("POST", "/api/auth/pair-request/AbCdEfGhIjKlMnOp/approve"));
    CHECK(lockedByNoAdmin("POST", "/api/auth/pair-request/AbCdEfGhIjKlMnOp/refuse"));
    CHECK(lockedByNoAdmin("POST", "/api/auth/pair-request/AbCdEfGhIjKlMnOp/approve", true));
    CHECK(!lockedByNoAdmin("GET", "/api/auth/pair-request/AbCdEfGhIjKlMnOp"));

    // What the booth is for, allowed: read, wake, play, stop, leave.
    CHECK(!lockedByNoAdmin("GET", "/api/hosts"));
    CHECK(!lockedByNoAdmin("GET", "/api/hosts/uuid-1/apps"));
    CHECK(!lockedByNoAdmin("POST", "/api/hosts/uuid-1/wol"));
    CHECK(!lockedByNoAdmin("POST", "/api/hosts/uuid-1/start"));
    CHECK(!lockedByNoAdmin("POST", "/api/hosts/uuid-1/quit"));
    CHECK(!lockedByNoAdmin("POST", "/api/hosts/uuid-1/stop-session"));
    // And what those are signed over (LaunchGate).
    CHECK(!lockedByNoAdmin("POST", "/api/hosts/uuid-1/launch-proof"));
    CHECK(!lockedByNoAdmin("POST", "/api/hosts/uuid-1/quit-ticket"));
    CHECK(!lockedByNoAdmin("POST", "/api/auth/logout"));
    CHECK(!lockedByNoAdmin("POST", "/api/share/player/join"));
    CHECK(!lockedByNoAdmin("POST", "/api/logs/client"));

    // share_in_booth_mode (issue #40): the share slots and a guest's PIN come
    // back, and nothing else does.
    CHECK(lockedByNoAdmin("POST", "/api/share/slots/2/activate", false));
    CHECK(lockedByNoAdmin("POST", "/api/share/player/pin", false));
    CHECK(!lockedByNoAdmin("POST", "/api/share/slots/2/activate", true));
    CHECK(!lockedByNoAdmin("POST", "/api/share/slots/2/permissions", true));
    CHECK(!lockedByNoAdmin("POST", "/api/share/slots/2/deactivate", true));
    CHECK(!lockedByNoAdmin("POST", "/api/share/player/pin", true));
    CHECK(lockedByNoAdmin("POST", "/api/auth/validate", true));
    CHECK(lockedByNoAdmin("POST", "/api/auth/admin-unlock", true));
    CHECK(lockedByNoAdmin("POST", "/api/auth/regenerate", true));
    CHECK(lockedByNoAdmin("POST", "/api/auth/pin/start", true));
    CHECK(lockedByNoAdmin("POST", "/api/hosts/manual", true));
    CHECK(lockedByNoAdmin("POST", "/api/hosts/uuid-1/pair", true));
    CHECK(lockedByNoAdmin("POST", "/api/update/start", true));
    CHECK(lockedByNoAdmin("POST", "/api/system/restart", true));

    // booth_stream_pin (issue #68): the stream PIN's routes, shut whatever the
    // verb unless the booth asked for them, share_in_booth_mode or not.
    for (const bool share : {false, true}) {
        CHECK(lockedByNoAdmin("POST", "/api/booth/join", share, false));
        CHECK(lockedByNoAdmin("POST", "/api/booth/pin", share, false));
        CHECK(lockedByNoAdmin("GET", "/api/booth/pin", share, false));
        CHECK(lockedByNoAdmin("HEAD", "/api/booth/anything", share, false));
        CHECK(lockedByNoAdmin("POST", "/api/booth", share, false));
        CHECK(!lockedByNoAdmin("POST", "/api/booth/join", share, true));
        CHECK(!lockedByNoAdmin("POST", "/api/booth/pin", share, true));
        // A whole segment, not a prefix: /api/boothx is another area.
        CHECK(!lockedByNoAdmin("GET", "/api/boothx/pin", share, false));
    }
    // With it on, the PIN replaces the links: no row is opened or read again,
    // no guest's PIN is asked, and a lifetime means nothing. What the one
    // streaming does to a guest who joined by the PIN goes on.
    for (const bool share : {false, true}) {
        CHECK(lockedByNoAdmin("POST", "/api/share/slots/2/activate", share, true));
        CHECK(lockedByNoAdmin("POST", "/api/share/slots/2/credentials", share, true));
        CHECK(lockedByNoAdmin("POST", "/api/share/slots/2/ttl", share, true));
        CHECK(lockedByNoAdmin("POST", "/api/share/slots/2/some-future-route", share, true));
        CHECK(lockedByNoAdmin("POST", "/api/share/slots", share, true));
        CHECK(lockedByNoAdmin("POST", "/api/share/player/pin", share, true));
        CHECK(!lockedByNoAdmin("POST", "/api/share/slots/2/permissions", share, true));
        CHECK(!lockedByNoAdmin("POST", "/api/share/slots/2/name", share, true));
        CHECK(!lockedByNoAdmin("POST", "/api/share/slots/2/deactivate", share, true));
        // The proof's challenge, the guest's own routes and the owner's reads.
        CHECK(!lockedByNoAdmin("POST", "/api/share/owner-proof", share, true));
        CHECK(!lockedByNoAdmin("POST", "/api/share/player/join", share, true));
        CHECK(!lockedByNoAdmin("POST", "/api/share/player/leave", share, true));
        CHECK(!lockedByNoAdmin("GET", "/api/share/status", share, true));
        // Nothing else of the booth opens with it.
        CHECK(lockedByNoAdmin("POST", "/api/auth/validate", share, true));
        CHECK(lockedByNoAdmin("POST", "/api/auth/pin/start", share, true));
        CHECK(lockedByNoAdmin("POST", "/api/hosts/manual", share, true));
        CHECK(lockedByNoAdmin("POST", "/api/hosts/uuid-1/pair", share, true));
        CHECK(lockedByNoAdmin("POST", "/api/server/stream-disconnect", share, true));
        CHECK(lockedByNoAdmin("POST", "/api/system/restart", share, true));
        CHECK(!lockedByNoAdmin("POST", "/api/hosts/uuid-1/start", share, true));
        CHECK(!lockedByNoAdmin("POST", "/api/hosts/uuid-1/stop-session", share, true));
    }

    SECTION("RequestGuard — this machine, not a host session over the public domain");

    {
        // A host-key session reaching us over the public domain, through a
        // tunnel that forwards from loopback, is "the host machine" (it may
        // stream the booth without a PIN), but it is a browser elsewhere:
        // localPrivilege, which HttpRequest::isThisMachine carries, stays false.
        Request r;
        r.method = "POST";
        r.path = "/api/hosts/uuid-1/start";
        r.headers = hdr({{"sec-fetch-site", "same-origin"},
                         {"host", "mw-abc123.example.com"},
                         {"content-type", "application/json"}});
        r.bodySize = 12;
        Context c;
        c.peerLocal = true;
        c.hostSession = true;
        c.publicDomain = "mw-abc123.example.com";
        const Decision d = evaluate(r, c);
        CHECK(d.outcome == Outcome::Allow);
        CHECK(d.hostMachine);
        CHECK(!d.localPrivilege);
    }

    SECTION("RequestGuard — the local channel is the host machine by its door, not by a name");

    {
        // What the channel hands over: no Host, no cookie, no address that is
        // this machine's, no admin key. It is the host machine and may
        // administer, writes included.
        Request r;
        r.method = "POST";
        r.path = "/api/admin/pin/generate";
        r.headers = hdr({{"content-type", "application/json"}});
        r.bodySize = 2;
        Context c;
        c.localChannel = true;
        const Decision d = evaluate(r, c);
        CHECK(d.outcome == Outcome::Allow);
        CHECK(d.localPrivilege);
        CHECK(d.hostMachine);
        CHECK(d.adminPrivilege);
        CHECK(!d.hostUntrusted);

        // The witness: the same request without the channel is nobody's.
        Context plain;
        const Decision without = evaluate(r, plain);
        CHECK(without.outcome == Outcome::Allow);
        CHECK(!without.localPrivilege);
        CHECK(!without.hostMachine);
        CHECK(!without.adminPrivilege);

        // Nothing a request carries moves it either way: a foreign Host, a
        // forwarding header. Its privilege is the channel's alone.
        r.headers = hdr({{"content-type", "application/json"},
                         {"host", "evil.example"},
                         {"x-forwarded-for", "203.0.113.7"}});
        const Decision odd = evaluate(r, c);
        CHECK(odd.hostMachine);
        CHECK(odd.adminPrivilege);
        CHECK(!odd.hostUntrusted);

        // --noadmin: still the host machine, and nobody administers.
        Context booth;
        booth.localChannel = true;
        booth.adminLocked = true;
        const Decision locked = evaluate(r, booth);
        CHECK(locked.hostMachine);
        CHECK(!locked.adminPrivilege);
        r.method = "GET";
        CHECK(!evaluate(r, booth).adminPrivilege);

        // The two checks every request goes through are kept as they are: a
        // body is JSON and says so, and another site's page is refused.
        Request untyped;
        untyped.method = "POST";
        untyped.path = "/api/admin/pin/generate";
        untyped.bodySize = 2;
        CHECK(evaluate(untyped, c).outcome == Outcome::BlockContentType);
        Request crossSite;
        crossSite.method = "GET";
        crossSite.path = "/api/server/status";
        crossSite.headers = hdr({{"sec-fetch-site", "cross-site"}});
        const Decision refused = evaluate(crossSite, c);
        CHECK(refused.outcome == Outcome::BlockCrossSite);
        CHECK(!refused.hostMachine);
        CHECK(!refused.adminPrivilege);
    }

    // What may be asked over the channel: the routes of the tray icon, the
    // command line and a second launch, each with its verb.
    CHECK(localChannelRoute("GET", "/api/health"));
    // A booth's stream PIN routes are never this channel's (issue #68): this
    // machine reads its own stream's PIN from its browser, not from a script.
    for (const char* method : {"GET", "POST"}) {
        CHECK(!localChannelRoute(method, "/api/booth/join"));
        CHECK(!localChannelRoute(method, "/api/booth/pin"));
        CHECK(!localChannelRoute(method, "/api/booth"));
    }
    // What this machine can capture and encode, for a script's request: the
    // read alone, never the routes that change its displays.
    CHECK(localChannelRoute("GET", "/api/native/status"));
    CHECK(!localChannelRoute("POST", "/api/native/status"));
    // And the state of its virtual display: the read of that one path, nothing
    // below it, and never the route that changes it.
    CHECK(localChannelRoute("GET", "/api/native/virtual-display"));
    CHECK(!localChannelRoute("POST", "/api/native/virtual-display"));
    CHECK(!localChannelRoute("GET", "/api/native/virtual-display/"));
    CHECK(!localChannelRoute("GET", "/api/native/virtual-display/enable"));
    CHECK(!localChannelRoute("GET", "/api/native/virtual-display/x/y"));
    CHECK(localChannelRoute("GET", "/api/server/status"));
    CHECK(localChannelRoute("GET", "/api/server/remote-link"));
    CHECK(localChannelRoute("GET", "/api/server/stream-activity"));
    CHECK(localChannelRoute("GET", "/api/setup/status"));
    CHECK(localChannelRoute("GET", "/api/auth/status"));
    CHECK(localChannelRoute("GET", "/api/internet/status"));
    CHECK(localChannelRoute("GET", "/api/internet/upnp-probe"));
    CHECK(localChannelRoute("POST", "/api/internet/enable"));
    CHECK(localChannelRoute("GET", "/api/settings/streaming"));
    CHECK(localChannelRoute("POST", "/api/settings/streaming"));
    CHECK(localChannelRoute("POST", "/api/admin/pin/generate"));
    CHECK(localChannelRoute("POST", "/api/admin/password"));
    CHECK(localChannelRoute("POST", "/api/system/restart"));
    CHECK(localChannelRoute("POST", "/api/system/quit"));
    // The tray's Disconnect (issue #42), asked by the icon of a service.
    CHECK(localChannelRoute("POST", "/api/server/stream-disconnect"));
    CHECK(!localChannelRoute("GET", "/api/server/stream-disconnect"));
    // The connection history: the administrator's, never the channel's.
    CHECK(!localChannelRoute("POST", "/api/admin/connection-history"));
    CHECK(localChannelRoute("POST", "/api/local/focus"));

    // And nothing else. The other verb of a listed route.
    CHECK(!localChannelRoute("POST", "/api/health"));
    CHECK(!localChannelRoute("GET", "/api/admin/pin/generate"));
    CHECK(!localChannelRoute("DELETE", "/api/settings/streaming"));
    CHECK(!localChannelRoute("get", "/api/health"));
    // A stream, a host, a pairing, a session, a key: none of this channel's business.
    CHECK(!localChannelRoute("POST", "/api/hosts/uuid-1/start"));
    CHECK(!localChannelRoute("GET", "/api/hosts"));
    CHECK(!localChannelRoute("POST", "/api/hosts/manual"));
    CHECK(!localChannelRoute("POST", "/api/auth/validate"));
    CHECK(!localChannelRoute("POST", "/api/auth/host-key"));
    CHECK(!localChannelRoute("POST", "/api/auth/pairing-key"));
    CHECK(!localChannelRoute("GET", "/api/admin/token"));
    CHECK(!localChannelRoute("GET", "/api/admin/settings"));
    CHECK(!localChannelRoute("POST", "/api/admin/settings"));
    CHECK(!localChannelRoute("GET", "/api/auth/sessions"));
    CHECK(!localChannelRoute("POST", "/api/share/slots/2/activate"));
    CHECK(!localChannelRoute("POST", "/api/update/start"));
    CHECK(!localChannelRoute("GET", "/api/logs/download"));
    // Not a page, not a file, not a socket.
    CHECK(!localChannelRoute("GET", "/"));
    CHECK(!localChannelRoute("GET", "/admin"));
    CHECK(!localChannelRoute("GET", "/index.html"));
    CHECK(!localChannelRoute("GET", "/ws"));
    CHECK(!localChannelRoute("GET", "/ws/control"));
    // The exact path: nothing under it, beside it or written another way.
    CHECK(!localChannelRoute("GET", "/api/health/"));
    CHECK(!localChannelRoute("GET", "/api/health/x"));
    CHECK(!localChannelRoute("GET", "/api/server/status/../../admin/token"));
    CHECK(!localChannelRoute("GET", "/API/HEALTH"));
    CHECK(!localChannelRoute("GET", "/api/healthz"));
    CHECK(!localChannelRoute("GET", ""));

    // Who the local channel lets in (localCallerVerdict), from what its server
    // end read of the caller's token. SIDs as LocalChannel's sidText() writes them.
    const QString serverSid = QStringLiteral("S-1-5-21-1111-2222-3333-1001");
    const QString otherSid = QStringLiteral("S-1-5-21-1111-2222-3333-1002");
    const QString systemSid = QStringLiteral("S-1-5-18");
    const QString adminsSid = QStringLiteral("S-1-5-32-544");
    const TokenGroup usersGroup{QStringLiteral("S-1-5-32-545"), kGroupEnabled};
    // As an elevated administrator's token states it (mandatory, by default, enabled, owner)…
    const TokenGroup adminsEnabled{adminsSid, 0x1 | 0x2 | kGroupEnabled | 0x8};
    // …and as UAC leaves it in that administrator's other programs.
    const TokenGroup adminsDenyOnly{adminsSid, kGroupDenyOnly};
    auto caller = [](const QString& sid, const QList<TokenGroup>& groups) {
        LocalCaller c;
        c.readable = true;
        c.sid = sid;
        c.groups = groups;
        c.integrityRid = kMediumIntegrity;
        return c;
    };

    SECTION("RequestGuard — the local channel lets in the server's account, and a service its "
            "administrators too");
    {
        for (bool service : {false, true}) {
            // The server's own account, whatever its groups.
            CHECK(localCallerVerdict(service, serverSid, adminsSid,
                                     caller(serverSid, {usersGroup})) == LocalVerdict::Admit);
            // Another account, with no administrators' group: never.
            CHECK(
                localCallerVerdict(service, serverSid, adminsSid, caller(otherSid, {usersGroup})) ==
                LocalVerdict::StandardAccount);
        }
        // An administrator of this PC, for a service: elevated or not.
        CHECK(localCallerVerdict(true, serverSid, adminsSid,
                                 caller(otherSid, {usersGroup, adminsEnabled})) ==
              LocalVerdict::Admit);
        CHECK(localCallerVerdict(true, serverSid, adminsSid,
                                 caller(otherSid, {usersGroup, adminsDenyOnly})) ==
              LocalVerdict::Admit);
        // Never for a server in its owner's session: its channel is that account's.
        CHECK(localCallerVerdict(false, serverSid, adminsSid, caller(otherSid, {adminsEnabled})) ==
              LocalVerdict::StandardAccount);
        CHECK(localCallerVerdict(false, serverSid, adminsSid, caller(otherSid, {adminsDenyOnly})) ==
              LocalVerdict::StandardAccount);
        // The group stated neither enabled nor for deny only: not counted.
        CHECK(localCallerVerdict(true, serverSid, adminsSid,
                                 caller(otherSid, {TokenGroup{adminsSid, 0x1 | 0x2}})) ==
              LocalVerdict::StandardAccount);
        // A group that is not the administrators', however it is stated.
        CHECK(localCallerVerdict(
                  true, serverSid, adminsSid,
                  caller(otherSid, {TokenGroup{QStringLiteral("S-1-5-32-5440"), kGroupEnabled}})) ==
              LocalVerdict::StandardAccount);
        // The system's own token holds the administrators, enabled: in for a
        // service, as an administrator; a server in its owner's session, never.
        CHECK(localCallerVerdict(true, serverSid, adminsSid, caller(systemSid, {adminsEnabled})) ==
              LocalVerdict::Admit);
        CHECK(localCallerVerdict(false, serverSid, adminsSid, caller(systemSid, {adminsEnabled})) ==
              LocalVerdict::StandardAccount);
        // A service that runs as the system: the system is its own account.
        CHECK(localCallerVerdict(true, systemSid, adminsSid, caller(systemSid, {})) ==
              LocalVerdict::Admit);
        // The local service, the network service, the anonymous: accounts like any other.
        for (const char* sid : {"S-1-5-19", "S-1-5-20", "S-1-5-7"})
            CHECK(localCallerVerdict(true, serverSid, adminsSid,
                                     caller(QString::fromLatin1(sid), {usersGroup})) ==
                  LocalVerdict::StandardAccount);
        // Text that is not the server's SID as sidText() writes it is another account.
        CHECK(localCallerVerdict(true, serverSid, adminsSid,
                                 caller(serverSid.toLower(), {usersGroup})) ==
              LocalVerdict::StandardAccount);
        // The administrators' SID not read: nobody is let in as one.
        CHECK(localCallerVerdict(true, serverSid, QString(), caller(otherSid, {adminsEnabled})) ==
              LocalVerdict::StandardAccount);
        CHECK(localCallerVerdict(true, serverSid, QString(), caller(serverSid, {})) ==
              LocalVerdict::Admit);
    }

    SECTION("RequestGuard — a sandbox is shut out of the local channel, whoever it belongs to");
    {
        const QList<LocalCaller> welcome = {caller(serverSid, {usersGroup}),
                                            caller(otherSid, {usersGroup, adminsEnabled}),
                                            caller(otherSid, {usersGroup, adminsDenyOnly})};
        for (const LocalCaller& base : welcome) {
            CHECK(localCallerVerdict(true, serverSid, adminsSid, base) == LocalVerdict::Admit);
            for (quint32 level : {0x0000u, 0x1000u, 0x1FFFu}) { // untrusted, low, just below medium
                LocalCaller c = base;
                c.integrityRid = level;
                CHECK(localCallerVerdict(true, serverSid, adminsSid, c) == LocalVerdict::Sandboxed);
                CHECK(localCallerVerdict(false, serverSid, adminsSid, c) ==
                      LocalVerdict::Sandboxed);
            }
            LocalCaller container = base;
            container.appContainer = true;
            CHECK(localCallerVerdict(true, serverSid, adminsSid, container) ==
                  LocalVerdict::Sandboxed);
            CHECK(localCallerVerdict(false, serverSid, adminsSid, container) ==
                  LocalVerdict::Sandboxed);
            LocalCaller restricted = base;
            restricted.restricted = true;
            CHECK(localCallerVerdict(true, serverSid, adminsSid, restricted) ==
                  LocalVerdict::Sandboxed);
            CHECK(localCallerVerdict(false, serverSid, adminsSid, restricted) ==
                  LocalVerdict::Sandboxed);
        }
        // Above medium is no sandbox: an elevated program, the system.
        for (quint32 level : {0x2000u, 0x2100u, 0x3000u, 0x4000u}) {
            LocalCaller c = caller(serverSid, {});
            c.integrityRid = level;
            CHECK(localCallerVerdict(false, serverSid, adminsSid, c) == LocalVerdict::Admit);
        }
    }

    SECTION("RequestGuard — a caller of the local channel that cannot be read is shut out");
    {
        for (bool service : {false, true}) {
            // Nothing read.
            CHECK(localCallerVerdict(service, serverSid, adminsSid, LocalCaller{}) ==
                  LocalVerdict::Unreadable);
            // Said read, with no account: not read.
            LocalCaller noAccount = caller(QString(), {adminsEnabled});
            CHECK(localCallerVerdict(service, serverSid, adminsSid, noAccount) ==
                  LocalVerdict::Unreadable);
            // Facts there, but not marked read: not read, even the server's own.
            LocalCaller unmarked = caller(serverSid, {adminsEnabled});
            unmarked.readable = false;
            CHECK(localCallerVerdict(service, serverSid, adminsSid, unmarked) ==
                  LocalVerdict::Unreadable);
            // The server's own account not read: nobody is let in.
            CHECK(localCallerVerdict(service, QString(), adminsSid, caller(serverSid, {})) ==
                  LocalVerdict::Unreadable);
            CHECK(localCallerVerdict(service, QString(), adminsSid,
                                     caller(otherSid, {adminsEnabled})) ==
                  LocalVerdict::Unreadable);
        }
    }

    SECTION("RequestGuard — the local channel asks who before what, on every one of its routes");
    {
        const QList<LocalRoute> routes = localChannelRoutes();
        CHECK(routes.size() >= 19);
        // The ones that matter most are there, so the walk below covers them.
        auto listed = [&routes](const char* method, const char* path) {
            for (const LocalRoute& r : routes)
                if (QLatin1String(r.method) == QLatin1String(method) &&
                    QLatin1String(r.path) == QLatin1String(path))
                    return true;
            return false;
        };
        CHECK(listed("GET", "/api/server/remote-link"));
        CHECK(listed("POST", "/api/admin/pin/generate"));
        CHECK(listed("POST", "/api/admin/password"));
        CHECK(listed("POST", "/api/system/quit"));

        for (const LocalRoute& r : routes) {
            const QString method = QString::fromLatin1(r.method);
            const QString path = QString::fromLatin1(r.path);
            CHECK(localChannelRoute(method, path));
            CHECK(localChannelGate(method, path, LocalVerdict::Admit) == LocalGate::Pass);
            for (LocalVerdict refused :
                 {LocalVerdict::StandardAccount, LocalVerdict::Sandboxed, LocalVerdict::Unreadable})
                CHECK(localChannelGate(method, path, refused) == LocalGate::Forbidden);
        }
        // Not a route of the channel: 404 for who is let in, and the same 403
        // as for every route to anyone else, who learns nothing of the list.
        CHECK(localChannelGate("GET", "/api/admin/token", LocalVerdict::Admit) ==
              LocalGate::NotFound);
        CHECK(localChannelGate("GET", "/api/admin/token", LocalVerdict::StandardAccount) ==
              LocalGate::Forbidden);
        CHECK(localChannelGate("POST", "/api/health", LocalVerdict::Sandboxed) ==
              LocalGate::Forbidden);
        CHECK(localChannelGate("GET", "", LocalVerdict::Unreadable) == LocalGate::Forbidden);
    }

    SECTION("RequestGuard — a session on another browser's key");

    {
        // HttpServer::tunnelKeyRefused: a session cookie over a tunnel whose
        // browser key is not the one the session was bound to. The key cannot
        // leave the browser it was made in, so the cookie did — copied out of
        // the jar it is kept in, and replayed from somewhere else.
        const QByteArray keyA("spki-of-browser-a");
        const QByteArray keyB("spki-of-browser-b");
        CHECK(!tunnelKeyRefused(true, keyA, keyA));
        CHECK(tunnelKeyRefused(true, keyB, keyA));
        // The tunnel names no key, and the session has one: refused too.
        CHECK(tunnelKeyRefused(true, QByteArray(), keyA));
        // A session older than key binding is let through, as before.
        CHECK(!tunnelKeyRefused(true, keyB, QByteArray()));
        // Off the tunnel there is no browser key to hold the session against.
        CHECK(!tunnelKeyRefused(false, QByteArray(), keyA));
        // A signalling socket opened through the tunnel carries the key alone.
        CHECK(tunnelKeyRefused(false, keyB, keyA));
    }

    SECTION("RequestGuard — the admin key matches itself, and nothing else");

    {
        const QByteArray key("0123456789abcdef0123456789abcdef");
        CHECK(secretMatches(key, key));
        CHECK(!secretMatches(QByteArray("0123456789abcdef0123456789abcdee"), key));
        CHECK(!secretMatches(key.left(31), key));
        CHECK(!secretMatches(QByteArray(), key));
        // Longer by any amount is another key, whatever the extra bytes are.
        for (int extra : {1, 255, 256, 512}) {
            CHECK(!secretMatches(key + QByteArray(extra, '\0'), key));
            CHECK(!secretMatches(key + QByteArray(extra, 'x'), key));
        }
        CHECK(!secretMatches(key, key + QByteArray(256, '\0')));
        // No key set: nothing matches, not even nothing.
        CHECK(!secretMatches(QByteArray(), QByteArray()));
        CHECK(!secretMatches(key, QByteArray()));
    }

    SECTION("RequestGuard — a pipe's server is this install's only when everything read says so "
            "(serverVerdict)");
    {
        const QString me = QStringLiteral("S-1-5-21-1111-2222-3333-1001");
        const QString other = QStringLiteral("S-1-5-21-1111-2222-3333-1002");
        const QString admins = QStringLiteral("S-1-5-32-544");
        const QString system = QLatin1String(kSystemSid);
        const QString exe = QStringLiteral("C:\\Program Files\\MoonlightWeb\\MoonlightWeb.exe");
        // A service, as a standard account reads it: the system's pipe, no
        // token to open, session 0, a locked folder, this same executable.
        auto service = [&]() {
            ServerFacts f;
            f.pipeOwnerSid = system;
            f.sessionRead = true;
            f.sessionId = 0;
            f.image = exe;
            f.ownImage = exe;
            f.installDirLocked = true;
            return f;
        };
        // This account's own server, in its session.
        auto mine = [&]() {
            ServerFacts f;
            f.pipeOwnerSid = me;
            f.tokenUserSid = me;
            f.sessionRead = true;
            f.sessionId = 1;
            f.image = exe;
            f.ownImage = exe;
            return f;
        };
        struct Row
        {
            const char* what;
            ServerFacts facts;
            ServerVerdict expected;
        };
        std::vector<Row> rows;
        rows.push_back({"a service, token unread", service(), ServerVerdict::Ours});
        {
            ServerFacts f = service();
            f.tokenUserSid = system;
            rows.push_back({"a service, token read", f, ServerVerdict::Ours});
        }
        rows.push_back({"this account's server", mine(), ServerVerdict::Ours});
        {
            ServerFacts f = mine();
            f.pipeOwnerSid = admins;
            rows.push_back({"this account's server, elevated", f, ServerVerdict::Ours});
        }
        {
            ServerFacts f = service();
            f.pipeOwnerSid = other;
            rows.push_back({"an impostor's pipe, another account", f, ServerVerdict::OtherOwner});
        }
        {
            ServerFacts f = service();
            f.pipeOwnerSid.clear();
            rows.push_back({"the pipe's owner unread", f, ServerVerdict::Unreadable});
        }
        // A standard account may not open a service's process: the pipe alone.
        {
            ServerFacts f = service();
            f.image.clear();
            rows.push_back({"a service whose image is unread", f, ServerVerdict::Ours});
        }
        {
            ServerFacts f = service();
            f.image.clear();
            f.sessionId = 2;
            rows.push_back(
                {"image unread, a system pipe in session 2", f, ServerVerdict::NotSessionZero});
        }
        {
            ServerFacts f = service();
            f.image.clear();
            f.sessionRead = false;
            rows.push_back(
                {"image unread, the pipe's session unread", f, ServerVerdict::NotSessionZero});
        }
        {
            ServerFacts f = service();
            f.image.clear();
            f.installDirLocked = false;
            rows.push_back(
                {"image unread, this install's folder not locked", f, ServerVerdict::NotLocked});
        }
        {
            ServerFacts f = service();
            f.image.clear();
            f.pipeOwnerSid = other;
            rows.push_back({"image unread, another account's pipe", f, ServerVerdict::OtherOwner});
        }
        {
            ServerFacts f = service();
            f.image.clear();
            f.pipeOwnerSid = admins;
            rows.push_back({"image unread, an administrators' pipe, token unread", f,
                            ServerVerdict::Unreadable});
        }
        {
            ServerFacts f = mine();
            f.image.clear();
            rows.push_back({"this account's server, image unread", f, ServerVerdict::Unreadable});
        }
        {
            ServerFacts f = service();
            f.image = QStringLiteral("C:\\Program Files\\Other\\MoonlightWeb.exe");
            rows.push_back(
                {"a system program installed elsewhere", f, ServerVerdict::OtherProgram});
        }
        {
            ServerFacts f = mine();
            f.image = QStringLiteral("C:\\Users\\x\\Downloads\\MoonlightWeb.exe");
            rows.push_back({"this account's server, another exe", f, ServerVerdict::OtherProgram});
        }
        {
            ServerFacts f = service();
            f.image = exe.toUpper();
            rows.push_back({"the same path in other case", f, ServerVerdict::Ours});
        }
        {
            ServerFacts f = service();
            f.installDirLocked = false;
            rows.push_back(
                {"a system server in a folder others may write", f, ServerVerdict::NotLocked});
        }
        {
            ServerFacts f = service();
            f.sessionId = 2;
            rows.push_back(
                {"a system pipe, token unread, session 2", f, ServerVerdict::NotSessionZero});
        }
        {
            ServerFacts f = service();
            f.sessionRead = false;
            rows.push_back({"a system pipe, session unread", f, ServerVerdict::NotSessionZero});
        }
        {
            ServerFacts f = service();
            f.pipeOwnerSid = admins;
            rows.push_back(
                {"an administrators' pipe, token unread (Q3)", f, ServerVerdict::OtherAccount});
        }
        {
            ServerFacts f = service();
            f.pipeOwnerSid = admins;
            f.tokenUserSid = system;
            rows.push_back(
                {"the system's token, an administrators' pipe", f, ServerVerdict::OtherOwner});
        }
        {
            ServerFacts f = mine();
            f.tokenUserSid = other;
            rows.push_back(
                {"a pipe of mine, a server of another account", f, ServerVerdict::OtherAccount});
        }
        {
            ServerFacts f = mine();
            f.pipeOwnerSid = system;
            rows.push_back({"my token, the system's pipe", f, ServerVerdict::OtherOwner});
        }
        {
            ServerFacts f = mine();
            f.ownImage.clear();
            rows.push_back({"this program's own image unread", f, ServerVerdict::Unreadable});
        }
        for (const Row& row : rows) {
            const ServerVerdict got = serverVerdict(row.facts, me, admins);
            CHECK(got == row.expected);
            if (got != row.expected)
                std::fprintf(stderr, "    serverVerdict: %s gave %d\n", row.what,
                             static_cast<int>(got));
        }
        // Nothing of this account read: only a service can be found.
        CHECK(serverVerdict(mine(), QString(), admins) == ServerVerdict::OtherOwner);
        CHECK(serverVerdict(service(), QString(), admins) == ServerVerdict::Ours);
        // The reason never names a path, an account or a number.
        for (ServerVerdict v : {ServerVerdict::Unreadable, ServerVerdict::OtherOwner,
                                ServerVerdict::OtherProgram, ServerVerdict::OtherAccount,
                                ServerVerdict::NotSessionZero, ServerVerdict::NotLocked}) {
            const QString text = serverVerdictText(v);
            CHECK(!text.isEmpty());
            CHECK(!text.contains(QLatin1Char('\\')) && !text.contains(QStringLiteral("S-1-")));
        }
        CHECK(serverVerdictText(ServerVerdict::OtherOwner) ==
              QStringLiteral("the channel belongs to another account"));
        CHECK(serverVerdictText(ServerVerdict::Ours).isEmpty());
    }

    SECTION("RequestGuard — a Unix socket's server is this install's only when everything read "
            "says so (unixServerVerdict)");
    {
        const QString exe = QStringLiteral("/opt/moonlightweb/bin/moonlightweb");
        // The machine's socket of a service that runs as root.
        auto machine = [&]() {
            UnixServerFacts f;
            f.machine = true;
            f.folderLocked = true;
            f.folderOwner = 0;
            f.socketOwner = 0;
            f.peerUid = 0;
            f.ownUid = 1000;
            return f;
        };
        // This account's socket, in its data folder.
        auto mine = [&]() {
            UnixServerFacts f;
            f.folderLocked = true;
            f.folderOwner = 1000;
            f.socketOwner = 1000;
            f.peerUid = 1000;
            f.ownUid = 1000;
            f.peerExe = exe;
            f.ownExe = exe;
            return f;
        };
        struct Row
        {
            const char* what;
            UnixServerFacts facts;
            ServerVerdict expected;
        };
        std::vector<Row> rows;
        rows.push_back({"a root service's machine socket", machine(), ServerVerdict::Ours});
        {
            UnixServerFacts f = machine();
            f.folderOwner = f.socketOwner = f.peerUid = 998;
            rows.push_back({"a service of its own account (User=)", f, ServerVerdict::Ours});
        }
        rows.push_back({"this account's server", mine(), ServerVerdict::Ours});
        {
            UnixServerFacts f = machine();
            f.folderLocked = false;
            rows.push_back(
                {"a machine folder someone else may write", f, ServerVerdict::NotLocked});
        }
        {
            UnixServerFacts f = machine();
            f.socketOwner = 1001;
            rows.push_back({"a machine socket of another account", f, ServerVerdict::OtherOwner});
        }
        {
            UnixServerFacts f = machine();
            f.peerUid = 1001;
            rows.push_back(
                {"a machine socket served by another account", f, ServerVerdict::OtherAccount});
        }
        {
            UnixServerFacts f = machine();
            f.peerUid = -1;
            rows.push_back({"a machine socket, the peer unread", f, ServerVerdict::Unreadable});
        }
        {
            UnixServerFacts f = machine();
            f.socketOwner = -1;
            rows.push_back({"not a socket, or a link", f, ServerVerdict::Unreadable});
        }
        {
            UnixServerFacts f = mine();
            f.socketOwner = 1001;
            rows.push_back({"my folder, another account's socket", f, ServerVerdict::OtherOwner});
        }
        {
            UnixServerFacts f = mine();
            f.folderLocked = false;
            rows.push_back({"my socket, a folder open to others", f, ServerVerdict::NotLocked});
        }
        {
            UnixServerFacts f = mine();
            f.peerUid = 0;
            rows.push_back({"my socket, served by root", f, ServerVerdict::OtherAccount});
        }
        {
            UnixServerFacts f = mine();
            f.peerExe.clear();
            rows.push_back({"my server, its executable unread", f, ServerVerdict::Unreadable});
        }
        {
            UnixServerFacts f = mine();
            f.peerExe = QStringLiteral("/tmp/moonlightweb");
            rows.push_back({"my socket, another program", f, ServerVerdict::OtherProgram});
        }
        for (const Row& row : rows) {
            const ServerVerdict got = unixServerVerdict(row.facts);
            CHECK(got == row.expected);
            if (got != row.expected)
                std::fprintf(stderr, "    unixServerVerdict: %s gave %d\n", row.what,
                             static_cast<int>(got));
        }
    }
}
