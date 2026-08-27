/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "server/AuthManager.h"
#include "server/AppSettings.h"

#include <QTemporaryDir>

void run_auth_manager_tests()
{
    SECTION("AuthManager");

    QTemporaryDir tmp;
    AppSettings settings;
    settings.m_FilePath = tmp.path() + "/settings.json";

    AuthManager auth(&settings);

    // ── PIN lifecycle ──────────────────────────────────────────────────────
    QString pin = auth.generatePin();
    CHECK(!pin.isEmpty());
    CHECK(auth.hasValidPin());

    // Valid PIN from a fresh IP.
    auto ok = auth.validatePin("198.51.100.10", pin);
    CHECK(ok.result == AuthManager::Valid);

    // clearPin invalidates.
    auth.clearPin();
    CHECK(!auth.hasValidPin());
    auth.generatePin();
    auth.regeneratePin(); // new pin + clears sessions

    // ── Rate limiting ──────────────────────────────────────────────────────
    const QString badIp = "203.0.113.7";
    bool sawRateLimited = false;
    for (int i = 0; i < 6; ++i) {
        auto r = auth.validatePin(badIp, "definitely-wrong-pin");
        if (r.result == AuthManager::RateLimited) sawRateLimited = true;
    }
    CHECK(sawRateLimited);
    CHECK(auth.isRateLimited(badIp));
    CHECK(auth.lockoutSeconds(badIp) > 0);
    CHECK(auth.failedAttemptCount(badIp) > 0);
    CHECK(auth.remainingAttempts(badIp) >= 0); // sane, non-negative count

    // ── Sessions ───────────────────────────────────────────────────────────
    QString token = auth.createSession("198.51.100.20", "Laptop");
    CHECK(!token.isEmpty());
    CHECK(auth.validateSession(token));
    CHECK(!auth.validateSession("bogus-token"));
    CHECK_EQ(auth.activeSessionCount(), 1);

    auth.touchSession(token);                                 // sliding expiration bump
    auth.setSessionStreaming(token, true);                    // single active stream flag
    auth.setSessionGeo(token, "Paris", "FR");                 // async geo result
    CHECK(auth.updateSessionAddress(token, "198.51.100.99")); // IP changed → true

    QList<SessionInfo> list = auth.sessions();
    CHECK_EQ(list.size(), 1);
    const QString id = list.first().token; // opaque id == token hash, not the raw cookie
    CHECK(id != token);
    CHECK(auth.renameSession(id, "Renamed"));
    CHECK(!auth.renameSession("unknown-id", "x"));

    // Persistence round-trip.
    auth.saveSessions();
    auth.loadSessions();

    // Revoking a session that is actively streaming must request stream teardown.
    int revokedFired = 0;
    QObject::connect(&auth, &AuthManager::streamingSessionRevoked,
                     [&revokedFired]() { ++revokedFired; });
    auth.setSessionStreaming(token, true); // re-flag: streaming is runtime-only, reset by load
    auth.destroySession(id);
    CHECK(!auth.validateSession(token)); // gone after destroy by id
    CHECK_EQ(revokedFired, 1);           // streaming session revoked → teardown signal

    auth.createSession("198.51.100.21");
    auth.destroyAllSessions();
    CHECK_EQ(auth.activeSessionCount(), 0);
    CHECK_EQ(revokedFired, 1); // non-streaming sessions → no teardown signal

    QString token3 = auth.createSession("198.51.100.22");
    auth.setSessionStreaming(token3, true);
    auth.destroyAllSessions();
    CHECK_EQ(revokedFired, 2);   // destroy-all with a streaming session → teardown signal
    auth.purgeExpiredSessions(); // no-op, just exercised

    // ── Session listing order and identity ─────────────────────────────────
    // The admin table re-renders on a timer, so the order must come from the
    // data and not from the hash layout: rows that move between two refreshes
    // send a click to the wrong device's Revoke button. Ties (same second) fall
    // back to the id, so the list is fully determined either way.
    const QString tokA = auth.createSession("198.51.100.30", "A");
    const QString tokB = auth.createSession("198.51.100.31", "B");
    const QString tokC = auth.createSession("198.51.100.32", "C");
    QList<SessionInfo> ordered = auth.sessions();
    CHECK_EQ(ordered.size(), 3);
    for (int i = 1; i < ordered.size(); ++i) {
        const bool sorted = ordered[i - 1].createdAt > ordered[i].createdAt ||
                            (ordered[i - 1].createdAt == ordered[i].createdAt &&
                             ordered[i - 1].token < ordered[i].token);
        CHECK(sorted);
    }
    CHECK_EQ(auth.sessions().first().token, ordered.first().token); // same list twice

    // The raw cookie token maps back to the id the admin UI revokes by, so the
    // caller's own row can be marked before it is clicked.
    const QString idB = auth.sessionIdForToken(tokB);
    CHECK(!idB.isEmpty());
    CHECK(idB != tokB); // the id is the hash, never the cookie value
    CHECK(auth.sessionIdForToken("bogus-token").isEmpty());
    CHECK(auth.sessionIdForToken(QString()).isEmpty());
    auth.destroySession(idB);
    CHECK(auth.sessionIdForToken(tokB).isEmpty()); // gone with its session
    auth.destroySession(auth.sessionIdForToken(tokA));
    auth.destroySession(auth.sessionIdForToken(tokC));
    CHECK_EQ(auth.activeSessionCount(), 0);

    // ── Logging yourself out ───────────────────────────────────────────────
    // A visitor holds nothing but the raw cookie, so that is what logoutSession
    // takes — and it can only ever reach their own session. Without this, the
    // only way out is the admin page, which someone in an internet café cannot
    // open: they would walk away leaving a 90-day session on a public machine.
    const QString mine = auth.createSession("198.51.100.40", "Bar PC");
    const QString theirs = auth.createSession("198.51.100.41", "Someone else");
    CHECK(auth.logoutSession(mine));
    CHECK(!auth.validateSession(mine));
    CHECK(auth.validateSession(theirs));   // the neighbour is untouched
    CHECK(!auth.logoutSession(mine));      // already gone
    CHECK(!auth.logoutSession(QString())); // no cookie at all
    CHECK(!auth.logoutSession("bogus-token"));
    CHECK(!auth.logoutSession(auth.sessionIdForToken(theirs))); // the id is not a cookie
    CHECK(auth.validateSession(theirs));                        // …so it revokes nothing
    auth.destroyAllSessions();

    // ── "Remember me" declined ─────────────────────────────────────────────
    // Same session object, two differences: it never reaches the disk, and it
    // expires in hours rather than months.
    const QString temp = auth.createSession("198.51.100.42", "Kiosk", false, /*ephemeral=*/true);
    CHECK(auth.validateSession(temp));
    CHECK(auth.isEphemeralSession(temp));
    CHECK_EQ(auth.sessions().size(), 1);
    CHECK(auth.sessions().first().ephemeral);

    const QString kept = auth.createSession("198.51.100.43", "Home PC");
    CHECK(!auth.isEphemeralSession(kept));
    CHECK(!auth.isEphemeralSession("bogus-token"));

    // The round-trip is the assertion: only the remembered session comes back.
    auth.saveSessions();
    auth.loadSessions();
    CHECK_EQ(auth.activeSessionCount(), 1);
    CHECK(!auth.sessions().first().ephemeral);
    CHECK_EQ(auth.sessions().first().machineName, QStringLiteral("Home PC"));
    auth.destroyAllSessions();

    // ── Certificate auth ───────────────────────────────────────────────────
    QString cert = auth.generateCertificateToken();
    CHECK(!cert.isEmpty());
    CHECK_EQ(auth.certificateToken(), cert);
    CHECK(auth.validateCertificate(cert));
    CHECK(!auth.validateCertificate("not-the-cert"));
    auth.setCertAuthEnabled(true);
    CHECK(auth.certAuthEnabled());

    // ── Remote admin password ──────────────────────────────────────────────
    // Out of the box the door is SHUT: remote administration is enabled, but no
    // password exists and there is no built-in default to fall back on, so
    // nothing an attacker can type unlocks anything.
    const QString adminIp = "192.168.5.20";
    CHECK(auth.remoteAdminEnabled());
    CHECK(!auth.adminPasswordSet());
    CHECK(settings.adminPasswordDigest().isEmpty()); // nothing stored yet
    CHECK(auth.validateAdminPassword(adminIp, "moonlightweb").result == AuthManager::InvalidPin);
    CHECK(auth.validateAdminPassword(adminIp, "").result == AuthManager::InvalidPin);

    CHECK(!auth.setAdminPassword("short")); // below MIN_ADMIN_PASSWORD_LEN
    CHECK(!auth.adminPasswordSet());

    CHECK(auth.setAdminPassword("correct-horse"));
    CHECK(auth.adminPasswordSet());

    // The plaintext is never stored: what lands in settings is a PBKDF2 digest.
    const QString digest = settings.adminPasswordDigest();
    CHECK(digest.startsWith("pbkdf2-sha256$"));
    CHECK(!digest.contains("correct-horse"));

    CHECK(auth.validateAdminPassword(adminIp, "correct-horse").result == AuthManager::Valid);
    CHECK(auth.validateAdminPassword(adminIp, "wrong").result == AuthManager::InvalidPin);

    // Its lockout counter is its own: hammering the password must not lock the
    // same address out of PIN login.
    const QString pinIp = "192.168.5.21";
    auth.generatePin();
    for (int i = 0; i < 6; ++i)
        auth.validateAdminPassword(pinIp, "wrong");
    CHECK(auth.isRateLimited(pinIp) == false);
    CHECK(auth.validateAdminPassword(pinIp, "correct-horse").result == AuthManager::RateLimited);

    // A session is promoted, not created: the password raises what an
    // already-admitted device may do.
    QString adminToken = auth.createSession(adminIp, "Living room PC");
    CHECK(!auth.isAdminSession(adminToken));
    CHECK(auth.promoteSessionToAdmin(adminToken));
    CHECK(auth.isAdminSession(adminToken));
    CHECK(!auth.isHostSession(adminToken)); // admin, but not the host machine
    CHECK(!auth.promoteSessionToAdmin("bogus-token"));

    // How a host session was born decides what it is worth afterwards. A host
    // session created on a socket must NOT be honoured on the tunnel — a token
    // minted at the machine says nothing about who holds it at the far end of a
    // relayed connection — and HttpServer asks this question to tell them apart.
    // Only a session created BY a tunnel redemption answers yes.
    {
        const QString onSocket = auth.createSession("127.0.0.1", "Host machine", /*isHost=*/true);
        CHECK(auth.isHostSession(onSocket));
        CHECK(!auth.isTunnelHostSession(onSocket));

        const QString onTunnel = auth.createSession("192.168.1.44", "Host machine (remote link)",
                                                   /*isHost=*/true, /*ephemeral=*/false,
                                                   /*viaTunnel=*/true);
        CHECK(auth.isHostSession(onTunnel));
        CHECK(auth.isTunnelHostSession(onTunnel));

        // A plain session is neither, whatever it claims.
        CHECK(!auth.isTunnelHostSession(adminToken));
        CHECK(!auth.isTunnelHostSession("bogus-token"));

        // It has to survive a restart, or the owner would silently lose admin
        // the first time the server came back.
        auth.saveSessions();
        auth.loadSessions();
        CHECK(auth.isTunnelHostSession(onTunnel));
        CHECK(!auth.isTunnelHostSession(onSocket));
    }

    // Changing the password revokes every unlock the old one bought.
    CHECK(auth.setAdminPassword("another-password"));
    CHECK(!auth.isAdminSession(adminToken));
    CHECK(auth.validateSession(adminToken)); // still signed in, just not admin

    // Disabling closes the door entirely — the stored password stops being
    // accepted — and a stale flag on disk is not honoured on the way back in.
    auth.promoteSessionToAdmin(adminToken);
    auth.setRemoteAdminEnabled(false);
    CHECK(!auth.isAdminSession(adminToken));
    CHECK(auth.validateAdminPassword("192.168.5.30", "another-password").result ==
          AuthManager::InvalidPin);
    auth.saveSessions();
    auth.loadSessions();
    CHECK(!auth.isAdminSession(adminToken));

    // Re-enabling restores the operator's password: the digest survived the
    // round trip.
    auth.setRemoteAdminEnabled(true);
    CHECK(auth.adminPasswordSet());
    CHECK(auth.validateAdminPassword("192.168.5.32", "another-password").result ==
          AuthManager::Valid);
    auth.destroyAllSessions();

    // ── LAN address classification (gates the unlock) ──────────────────────
    CHECK(AuthManager::isLanAddress("127.0.0.1"));
    CHECK(AuthManager::isLanAddress("::1"));
    CHECK(AuthManager::isLanAddress("192.168.1.5"));
    CHECK(AuthManager::isLanAddress("10.1.2.3"));
    CHECK(AuthManager::isLanAddress("172.20.0.1"));
    CHECK(AuthManager::isLanAddress("::ffff:192.168.1.5")); // IPv4-mapped
    CHECK(AuthManager::isLanAddress("169.254.3.4"));        // link-local
    CHECK(AuthManager::isLanAddress("fe80::1"));            // link-local v6
    CHECK(AuthManager::isLanAddress("fd12:3456::1"));       // unique-local v6
    CHECK(!AuthManager::isLanAddress("8.8.8.8"));
    CHECK(!AuthManager::isLanAddress("172.32.0.1")); // just outside 172.16/12
    CHECK(!AuthManager::isLanAddress("2001:db8::1"));
    CHECK(!AuthManager::isLanAddress(""));

    // ── Address helpers (static) ───────────────────────────────────────────
    CHECK_EQ(AuthManager::cleanClientAddress("::ffff:192.168.1.5"), QString("192.168.1.5"));
    CHECK_EQ(AuthManager::isPrivateIP("192.168.1.5"), QString("Local"));
    CHECK_EQ(AuthManager::isPrivateIP("10.0.0.1"), QString("Local"));
    CHECK_EQ(AuthManager::isPrivateIP("8.8.8.8"), QString("Remote"));
    CHECK_EQ(AuthManager::rateLimitKey("8.8.8.8"), QString("8.8.8.8"));   // IPv4 = raw
    CHECK(!AuthManager::rateLimitKey("2001:db8:abcd:1234::1").isEmpty()); // IPv6 = /64-ish
}
