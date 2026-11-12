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
}
