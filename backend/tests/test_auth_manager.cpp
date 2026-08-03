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
