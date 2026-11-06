/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "common/Logger.h"
#include "common/PairingCrypto.h"
#include "common/Types.h"
#include "server/AuthManager.h"
#include "server/AppSettings.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
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

    // The PIN itself and nothing more: whatever follows it, or comes before.
    for (int pad : {1, 255, 256, 512, 1024}) {
        const QString ip = QString("198.51.100.%1").arg(100 + pad % 100);
        CHECK(auth.validatePin(ip, pin + QString(pad, QChar(0))).result != AuthManager::Valid);
        CHECK(auth.validatePin(ip, QString(pad, QChar(0)) + pin).result != AuthManager::Valid);
    }
    CHECK(auth.validatePin("198.51.100.11", pin.left(pin.size() - 1)).result != AuthManager::Valid);
    CHECK(auth.validatePin("198.51.100.11", pin).result == AuthManager::Valid);

    // The PIN made after one was used is shown to nobody, and opens nothing
    // until the administrator asks for a PIN again.
    {
        AppSettings used;
        used.m_FilePath = tmp.path() + "/used-pin.json";
        AuthManager a(&used);
        const QString first = a.generatePin();
        CHECK(a.validatePin("198.51.100.30", first).result == AuthManager::Valid);
        a.autoRegeneratePin();
        CHECK(a.isPinConsumed());
        CHECK(!a.hasValidPin());
        CHECK(a.validatePin("198.51.100.31", a.currentPin()).result != AuthManager::Valid);
        CHECK(a.validatePin("198.51.100.32", first).result != AuthManager::Valid);
        const QString fresh = a.generatePin();
        CHECK(a.hasValidPin());
        CHECK(a.validatePin("198.51.100.33", fresh).result == AuthManager::Valid);
    }

    // A failure is forgiven once per 10 minutes without a new one, by the clock:
    // attempts from other addresses do not wind a bucket back faster.
    {
        AppSettings rl;
        rl.m_FilePath = tmp.path() + "/rate.json";
        AuthManager a(&rl);
        a.generatePin();
        qint64 now = 1'900'000'000;
        a.setRateClockForTesting([&now] { return now; });
        const QString target = "203.0.113.50";
        for (int i = 0; i < 3; ++i)
            a.validatePin(target, "wrong-pin");
        CHECK_EQ(a.failedAttemptCount(target), 3);
        CHECK(a.isRateLimited(target));
        now += 31;
        CHECK(!a.isRateLimited(target));

        int other = 1;
        const auto fromOthers = [&](int n) {
            for (int i = 0; i < n; ++i, ++other) {
                const QString ip = QString("198.18.%1.%2").arg(other / 250).arg(other % 250 + 1);
                a.validatePin(ip, "wrong-pin");
            }
        };
        fromOthers(20);
        CHECK_EQ(a.failedAttemptCount(target), 3); // under 10 minutes: nothing forgiven

        now += 600 - 31; // 10 minutes after the last failure
        fromOthers(20);
        CHECK_EQ(a.failedAttemptCount(target), 2);
        now += 599;
        fromOthers(20);
        CHECK_EQ(a.failedAttemptCount(target), 2);
        now += 1;
        fromOthers(1);
        CHECK_EQ(a.failedAttemptCount(target), 1);
        now += 3 * 600; // a long quiet: every step it covers, once
        fromOthers(1);
        CHECK_EQ(a.failedAttemptCount(target), 0);
        a.setRateClockForTesting({});
    }

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

    // What identifies a session stays out of the log, found or not.
    {
        QTemporaryDir logDir;
        const QString before = Logger::instance()->logFilePath();
        const QString logPath = logDir.path() + "/auth.log";
        Logger::instance()->setLogFile(logPath, false);
        auth.destroySession("canary-session-id");
        auth.renameSession("canary-session-id", "x");
        CHECK(auth.renameSession(id, "Renamed"));
        auth.setSessionGeo(token, "Paris", "FR");
        Logger::instance()->setLogFile(before);

        QFile written(logPath);
        CHECK(written.open(QIODevice::ReadOnly));
        const QByteArray lines = written.readAll();
        CHECK(lines.contains("no such session"));
        CHECK(!lines.contains("canary-session-id"));
        CHECK(!lines.contains(id.toUtf8()));
        CHECK(lines.contains("Geo data stored"));
        CHECK(!lines.contains(token.left(8).toUtf8()));
    }

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

    // ── The browser key a session is bound to (MW-BIND-v1) ─────────────────
    // What HttpServer::tunnelKeyRefused holds a tunnel's key against. A session
    // keeps the key it was bound to: the cookie copied out of the browser it
    // lives in cannot be bound again to the copier's own key.
    {
        const QByteArray keyA =
            PairingCrypto::publicKeySpkiFromPrivatePem(PairingCrypto::generatePrivateKeyPem());
        const QByteArray keyB =
            PairingCrypto::publicKeySpkiFromPrivatePem(PairingCrypto::generatePrivateKeyPem());
        const QString session = auth.createSession("198.51.100.50", "Phone");
        CHECK(auth.sessionPairingKey(session).isEmpty()); // like one older than binding
        CHECK(!auth.bindSessionKey(session, QByteArray("not a key")));
        CHECK(auth.bindSessionKey(session, keyA));
        CHECK(auth.bindSessionKey(session, keyA));  // the same browser, after a reconnect
        CHECK(!auth.bindSessionKey(session, keyB)); // another browser's key
        CHECK(auth.sessionPairingKey(session) == keyA);
        CHECK(!auth.bindSessionKey(QString(), keyA));
        CHECK(!auth.bindSessionKey("bogus-token", keyA));
        CHECK(auth.sessionPairingKey("bogus-token").isEmpty());
        auth.destroyAllSessions();
    }

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

    // The internet side of the door is shut out of the box, and opening it is
    // persisted like the rest.
    CHECK(!auth.remoteAdminInternet());
    CHECK(!settings.remoteAdminInternet());
    auth.setRemoteAdminInternet(true);
    CHECK(auth.remoteAdminInternet());
    CHECK(settings.remoteAdminInternet());

    // From the internet every address shares one more counter: three wrong
    // guesses from three addresses lock the fourth out as well. The LAN never
    // feeds that counter and keeps its own.
    CHECK(auth.validateAdminPassword("203.0.113.1", "wrong").result == AuthManager::InvalidPin);
    CHECK(auth.validateAdminPassword("203.0.113.2", "wrong").result == AuthManager::InvalidPin);
    {
        const AuthManager::ValidateResult third =
            auth.validateAdminPassword("203.0.113.3", "wrong");
        CHECK(third.result == AuthManager::InvalidPin);
        CHECK(third.lockoutSeconds > 0);
        CHECK_EQ(third.remainingAttempts, 2); // the shared counter's tier: 5 - 3
    }
    CHECK(auth.validateAdminPassword("203.0.113.4", "another-password").result ==
          AuthManager::RateLimited);
    CHECK(auth.validateAdminPassword("192.168.5.33", "another-password").result ==
          AuthManager::Valid);

    // Closing the internet side takes back every unlock: which ones it bought
    // is not recorded, and a LAN unlock is honoured from anywhere afterwards.
    {
        const QString office = auth.createSession("203.0.113.9", "Office laptop");
        CHECK(auth.promoteSessionToAdmin(office));
        auth.setRemoteAdminInternet(false);
        CHECK(!auth.isAdminSession(office));
        CHECK(auth.validateSession(office)); // still signed in, just not admin
        CHECK(!settings.remoteAdminInternet());
    }

    // Removing the password shuts the door again and takes back every unlock;
    // remote administration itself stays on, waiting for a new password.
    {
        const QString laptop = auth.createSession("192.168.5.40", "Laptop");
        CHECK(auth.promoteSessionToAdmin(laptop));
        auth.clearAdminPassword();
        CHECK(!auth.adminPasswordSet());
        CHECK(settings.adminPasswordDigest().isEmpty());
        CHECK(!auth.isAdminSession(laptop));
        CHECK(auth.validateSession(laptop));
        CHECK(auth.remoteAdminEnabled());
        CHECK(auth.validateAdminPassword("192.168.5.41", "another-password").result ==
              AuthManager::InvalidPin);
    }
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

    // ── Who may spend the password ─────────────────────────────────────────
    // On a socket both conditions are required, because either one alone is
    // forgeable in a deployment we support.
    CHECK(AuthManager::canUnlockAdmin("192.168.1.5", /*hostTrusted*/ true, /*viaTunnel*/ false));
    // A TLS-terminating tunnel on this machine forwards the whole internet from
    // loopback under our own name: local peer, trusted Host, still not a LAN
    // machine — which is why the peer check is not the only one…
    CHECK(!AuthManager::canUnlockAdmin("192.168.1.5", false, false));
    // …and a hairpin-NAT LAN client arrives under the public domain, which is
    // why the Host check is not the only one either.
    CHECK(!AuthManager::canUnlockAdmin("8.8.8.8", true, false));

    // Through the rendezvous the Host header names the introduction server, so
    // hostTrusted is false for every arrival and cannot gate anything. The
    // address is ICE's own conclusion instead: a LAN peer is admitted, the
    // internet is not.
    CHECK(AuthManager::canUnlockAdmin("192.168.1.5", false, /*viaTunnel*/ true));
    CHECK(AuthManager::canUnlockAdmin("fd12:3456::1", false, true));
    CHECK(!AuthManager::canUnlockAdmin("8.8.8.8", false, true));
    CHECK(!AuthManager::canUnlockAdmin("8.8.8.8", true, true));
    // A peer with no negotiated candidate pair is labelled, not addressed. It
    // parses as nothing and is refused on both paths.
    CHECK(!AuthManager::canUnlockAdmin("peer-4f2a", false, true));
    CHECK(!AuthManager::canUnlockAdmin("", true, true));

    // Once the owner opens the internet side: a public peer is admitted on
    // the rendezvous tunnel, where it holds a session earned with the PIN
    // over that link — never on a socket, where a proxy on this machine is
    // exactly what forwards the whole internet here.
    CHECK(AuthManager::canUnlockAdmin("8.8.8.8", false, /*viaTunnel*/ true, /*internet*/ true));
    CHECK(AuthManager::canUnlockAdmin("2001:db8::1", false, true, true));
    CHECK(!AuthManager::canUnlockAdmin("8.8.8.8", true, /*viaTunnel*/ false, true));
    CHECK(!AuthManager::canUnlockAdmin("8.8.8.8", false, true, /*internet*/ false));
    // A label is still nothing, and the LAN's own rules do not loosen.
    CHECK(!AuthManager::canUnlockAdmin("peer-4f2a", false, true, true));
    CHECK(!AuthManager::canUnlockAdmin("", false, true, true));
    CHECK(!AuthManager::canUnlockAdmin("192.168.1.5", false, false, true));
    CHECK(AuthManager::canUnlockAdmin("192.168.1.5", false, true, true));

    // ── Address helpers (static) ───────────────────────────────────────────
    CHECK_EQ(AuthManager::cleanClientAddress("::ffff:192.168.1.5"), QString("192.168.1.5"));
    CHECK_EQ(AuthManager::isPrivateIP("192.168.1.5"), QString("Local"));
    CHECK_EQ(AuthManager::isPrivateIP("10.0.0.1"), QString("Local"));
    CHECK_EQ(AuthManager::isPrivateIP("8.8.8.8"), QString("Remote"));
    CHECK_EQ(AuthManager::rateLimitKey("8.8.8.8"), QString("8.8.8.8"));   // IPv4 = raw
    CHECK(!AuthManager::rateLimitKey("2001:db8:abcd:1234::1").isEmpty()); // IPv6 = /64-ish

    // ── Whose attempt: behind a local reverse proxy, the visitor's ─────────
    {
        using H = QMap<QString, QString>;
        // No proxy headers: the peer, even on loopback.
        CHECK_EQ(AuthManager::attemptAddress("127.0.0.1", H{}), QString("127.0.0.1"));
        // Funnel / nginx: the LAST X-Forwarded-For entry is the proxy's own;
        // what a visitor wrote before it is theirs to forge.
        CHECK_EQ(AuthManager::attemptAddress("127.0.0.1",
                                             H{{"x-forwarded-for", "6.6.6.6, 203.0.113.9"}}),
                 QString("203.0.113.9"));
        CHECK_EQ(AuthManager::attemptAddress("::1", H{{"x-forwarded-for", "203.0.113.9"}}),
                 QString("203.0.113.9"));
        // cloudflared names the visitor outright, and wins.
        CHECK_EQ(AuthManager::attemptAddress("127.0.0.1", H{{"cf-connecting-ip", "198.51.100.7"},
                                                            {"x-forwarded-for", "10.0.0.1"}}),
                 QString("198.51.100.7"));
        // RFC 7239, quoted IPv6 with a port, IPv4 with a port.
        CHECK_EQ(AuthManager::attemptAddress(
                     "127.0.0.1", H{{"forwarded", "for=1.1.1.1, for=\"[2001:db8::7]:4711\""}}),
                 QString("2001:db8::7"));
        CHECK_EQ(AuthManager::attemptAddress("127.0.0.1",
                                             H{{"forwarded", "proto=https;for=192.0.2.60:80"}}),
                 QString("192.0.2.60"));
        // A LAN or internet peer is never believed: the header would buy a fresh
        // counter with every guess.
        CHECK_EQ(AuthManager::attemptAddress("192.168.1.20", H{{"x-forwarded-for", "9.9.9.9"}}),
                 QString("192.168.1.20"));
        CHECK_EQ(AuthManager::attemptAddress("8.8.8.8", H{{"cf-connecting-ip", "9.9.9.9"}}),
                 QString("8.8.8.8"));
        // Garbage in the header: the peer.
        CHECK_EQ(AuthManager::attemptAddress("127.0.0.1", H{{"x-forwarded-for", "not-an-ip"}}),
                 QString("127.0.0.1"));
    }

    // ── Home-screen handoff ────────────────────────────────────────────────
    {
        const QString owner = auth.createSession("198.51.100.40", "iOS Safari");
        const QString key = auth.homeScreenKey(owner);
        CHECK(key.startsWith(AuthManager::HOME_SCREEN_KEY_PREFIX));
        CHECK(key.size() > 40);
        // Every page load reads the manifest; the key stays the same meanwhile.
        CHECK_EQ(auth.homeScreenKey(owner), key);

        const QJsonArray machines{QJsonObject{{"id", "n95"}, {"name", "N95"}, {"url", "u"}}};
        auth.setHomeScreenInstances(owner, machines);

        // A host key, or anything else, is not a home-screen key.
        CHECK(!auth.redeemHomeScreenKey("not-a-key").has_value());
        CHECK(!auth.redeemHomeScreenKey(AuthManager::HOME_SCREEN_KEY_PREFIX + QString("x"))
                   .has_value());

        const auto handoff = auth.redeemHomeScreenKey(key);
        CHECK(handoff.has_value());
        if (handoff) {
            CHECK_EQ(handoff->machineName, QString("iOS Safari"));
            CHECK_EQ(handoff->instances, machines);
        }
        // Single-use.
        CHECK(!auth.redeemHomeScreenKey(key).has_value());

        // Spent, a new one is minted for the next shortcut.
        const QString second = auth.homeScreenKey(owner);
        CHECK(!second.isEmpty() && second != key);

        // Past its lifetime it is refused — and spent by the refusal.
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const QString late = auth.homeScreenKey(auth.createSession("198.51.100.41", "Old"),
                                                now - AuthManager::HOME_SCREEN_KEY_TTL_SECS - 5);
        CHECK(!auth.redeemHomeScreenKey(late, now).has_value());

        // Past half its life the manifest gets a fresh one.
        const QString aging = auth.createSession("198.51.100.42", "Aging");
        const QString first =
            auth.homeScreenKey(aging, now - AuthManager::HOME_SCREEN_KEY_TTL_SECS);
        CHECK(auth.homeScreenKey(aging, now) != first);

        // Revoking the session it came from revokes the key with it.
        auth.destroySession(auth.sessionIdForToken(owner));
        CHECK(!auth.redeemHomeScreenKey(second).has_value());

        // A visitor who declined "remember me" hands out no second session.
        const QString temporary = auth.createSession("198.51.100.43", "Kiosk", false, true);
        CHECK(auth.homeScreenKey(temporary).isEmpty());
        CHECK(auth.homeScreenKey(QString()).isEmpty());
    }

    // ── Who may know what this machine is called ───────────────────────────
    {
        const auto socket = [](const QString& address, const QString& host) {
            HttpRequest req;
            req.clientAddress = address;
            req.hostTrusted = true;
            req.headers.insert(QStringLiteral("host"), host);
            return req;
        };
        const QString ours = QStringLiteral("192.168.1.10:8443");

        // On this LAN, before any session.
        CHECK(AuthManager::mayKnowMachineName(socket("192.168.1.20", ours), false));
        // From the internet, once signed in only.
        CHECK(!AuthManager::mayKnowMachineName(socket("203.0.113.7", ours), false));
        CHECK(AuthManager::mayKnowMachineName(socket("203.0.113.7", ours), true));
        // A Host this machine does not go by is not this LAN.
        HttpRequest rebound = socket("192.168.1.20", "pc.example.test");
        rebound.hostTrusted = false;
        CHECK(!AuthManager::mayKnowMachineName(rebound, false));
        // The internet a proxy on this machine forwards from loopback.
        HttpRequest proxied = socket("127.0.0.1", "pc.example.test");
        CHECK(!AuthManager::mayKnowMachineName(proxied, false));
        proxied.headers.insert(QStringLiteral("x-forwarded-for"), QStringLiteral("203.0.113.50"));
        CHECK(!AuthManager::mayKnowMachineName(proxied, false));
        // The host machine itself, from loopback or with its host key from anywhere.
        HttpRequest host = socket("203.0.113.7", "pc.example.test");
        host.isHostMachine = true;
        CHECK(AuthManager::mayKnowMachineName(host, false));
        // On a line, by the address ICE settled on.
        HttpRequest line;
        line.viaTunnel = true;
        line.clientAddress = QStringLiteral("192.168.1.30");
        CHECK(AuthManager::mayKnowMachineName(line, false));
        line.clientAddress = QStringLiteral("198.51.100.9");
        CHECK(!AuthManager::mayKnowMachineName(line, false));
        CHECK(AuthManager::mayKnowMachineName(line, true));
    }
}
