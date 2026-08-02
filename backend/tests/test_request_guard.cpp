/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Covers the CSRF / DNS-rebinding gates. The threat these encode: the server
 * grants admin access on the source IP alone, and a browser running on the host
 * lends that IP to every page it has open — so "came from 127.0.0.1" must never
 * again be enough on its own.
 */
#include "test_framework.h"
#include "server/RequestGuard.h"

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
                         {"host", "192.168.1.9"},
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
                         {"host", "192.168.1.9"},
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
}
