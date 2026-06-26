/*
 * Moonlight-Web — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
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

    auth.touchSession(token);                                // sliding expiration bump
    auth.setSessionStreaming(token, true);                   // single active stream flag
    auth.setSessionGeo(token, "Paris", "FR");                // async geo result
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

    auth.destroySession(id);
    CHECK(!auth.validateSession(token)); // gone after destroy by id

    auth.createSession("198.51.100.21");
    auth.destroyAllSessions();
    CHECK_EQ(auth.activeSessionCount(), 0);
    auth.purgeExpiredSessions(); // no-op, just exercised

    // ── Certificate auth ───────────────────────────────────────────────────
    QString cert = auth.generateCertificateToken();
    CHECK(!cert.isEmpty());
    CHECK_EQ(auth.certificateToken(), cert);
    CHECK(auth.validateCertificate(cert));
    CHECK(!auth.validateCertificate("not-the-cert"));
    auth.setCertAuthEnabled(true);
    CHECK(auth.certAuthEnabled());

    // ── Address helpers (static) ───────────────────────────────────────────
    CHECK_EQ(AuthManager::cleanClientAddress("::ffff:192.168.1.5"), QString("192.168.1.5"));
    CHECK_EQ(AuthManager::isPrivateIP("192.168.1.5"), QString("Local"));
    CHECK_EQ(AuthManager::isPrivateIP("10.0.0.1"), QString("Local"));
    CHECK_EQ(AuthManager::isPrivateIP("8.8.8.8"), QString("Remote"));
    CHECK_EQ(AuthManager::rateLimitKey("8.8.8.8"), QString("8.8.8.8")); // IPv4 = raw
    CHECK(!AuthManager::rateLimitKey("2001:db8:abcd:1234::1").isEmpty()); // IPv6 = /64-ish
}
