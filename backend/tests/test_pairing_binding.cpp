/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * MW-BIND-v1 — the tests §9 of docs/design/pairing-signature.md asks for.
 *
 * The scenario throughout is the one the protocol exists to stop: something
 * relaying the signaling rewrites an `a=fingerprint` line so it can terminate
 * DTLS itself and inject keyboard and mouse events into the host's desktop.
 * Each check below is one way of attempting that, and each must fail closed.
 */

#include "test_framework.h"

#include "common/PairingCrypto.h"
#include "streaming/SdpFingerprint.h"

#include <QSet>
#include <QString>
#include <QStringList>

namespace {

/// 32 hex bytes, distinct per position so a shifted or truncated comparison
/// cannot pass by accident.
QString fingerprintOf(int seed)
{
    QStringList bytes;
    for (int i = 0; i < 32; ++i) {
        bytes << QStringLiteral("%1").arg((seed + i) & 0xFF, 2, 16, QLatin1Char('0')).toUpper();
    }
    return bytes.join(QLatin1Char(':'));
}

QString sdpWith(const QStringList& fingerprintLines)
{
    QStringList lines{QStringLiteral("v=0"),
                      QStringLiteral("o=- 4611731400430051336 2 IN IP4 127.0.0.1"),
                      QStringLiteral("s=-"),
                      QStringLiteral("t=0 0"),
                      QStringLiteral("m=application 9 UDP/DTLS/SCTP webrtc-datachannel"),
                      QStringLiteral("c=IN IP4 0.0.0.0"),
                      QStringLiteral("a=ice-ufrag:abcd")};
    lines += fingerprintLines;
    lines << QStringLiteral("a=setup:actpass");
    return lines.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n");
}

} // namespace

void run_pairing_binding_tests()
{
    using namespace PairingCrypto;

    // ── Fingerprint extraction ─────────────────────────────────────────────

    SECTION("MW-BIND — fingerprint extraction");

    const QString fp = fingerprintOf(0x10);
    CHECK_EQ(SdpFingerprint::extract(sdpWith({"a=fingerprint:sha-256 " + fp})), fp);

    // Legitimate and common: one identical line per m-section.
    CHECK_EQ(SdpFingerprint::extract(
                 sdpWith({"a=fingerprint:sha-256 " + fp, "a=fingerprint:sha-256 " + fp})),
             fp);

    // Case is normalised, so the same key always yields the same signed bytes.
    CHECK_EQ(SdpFingerprint::extract(sdpWith({"a=fingerprint:sha-256 " + fp.toLower()})), fp);

    SECTION("MW-BIND — SDP-level substitution attempts");

    // Leave the legitimate line in place and add your own: if we picked either,
    // the peer could well be using the other one.
    CHECK(SdpFingerprint::extract(sdpWith({"a=fingerprint:sha-256 " + fingerprintOf(0x40),
                                           "a=fingerprint:sha-256 " + fingerprintOf(0x99)}))
              .isEmpty());

    // A hash an attacker can collide is a hash they can bind to their own key.
    CHECK(
        SdpFingerprint::extract(sdpWith({"a=fingerprint:sha-1 " + fingerprintOf(0x50)})).isEmpty());

    CHECK(SdpFingerprint::extract(sdpWith({"a=fingerprint:sha-256 AB:CD"})).isEmpty());
    CHECK(SdpFingerprint::extract(sdpWith({"a=fingerprint:sha-256"})).isEmpty());
    CHECK(SdpFingerprint::extract(sdpWith({"a=fingerprint:sha-256 zz:" + fingerprintOf(1)}))
              .isEmpty());
    CHECK(SdpFingerprint::extract(sdpWith({})).isEmpty());
    CHECK(SdpFingerprint::extract(QString()).isEmpty());

    // ── Message framing and keys ───────────────────────────────────────────

    SECTION("MW-BIND — signed message framing");

    // Without length prefixes ("ab","c") and ("a","bc") would be identical
    // bytes — and one of the real fields is a fingerprint.
    CHECK(buildSignedMessage({"ab", "c"}) != buildSignedMessage({"a", "bc"}));

    SECTION("MW-BIND — key material");

    const QByteArray pem = generatePrivateKeyPem();
    CHECK(!pem.isEmpty());
    const QByteArray spki = publicKeySpkiFromPrivatePem(pem);
    CHECK(!spki.isEmpty());
    CHECK(isValidP256Spki(spki));
    CHECK(!keyId(spki).isEmpty());
    CHECK(!isValidP256Spki(QByteArray("not a key")));

    // WebCrypto speaks raw r‖s, not OpenSSL's native DER. A DER signature would
    // verify here and fail in every browser.
    CHECK_EQ(sign(pem, "payload").size(), SIGNATURE_BYTES);

    const QByteArray honestMsg = hostDigestInput("host-1", "key-1", "nonce", fingerprintOf(0x60));
    CHECK(verify(spki, honestMsg, sign(pem, honestMsg)));

    // ── The substitution attempts themselves ───────────────────────────────

    SECTION("MW-BIND — signature substitution and replay");

    // THE test: the relay swaps the fingerprint and forwards the original
    // signature untouched.
    {
        const QByteArray sig = sign(pem, honestMsg);
        const QByteArray tampered =
            hostDigestInput("host-1", "key-1", "nonce", fingerprintOf(0xAA));
        CHECK(!verify(spki, tampered, sig));
    }

    // The relay signs with a key of its own and hopes we never check whose.
    {
        const QByteArray attackerPem = generatePrivateKeyPem();
        CHECK(!verify(spki, honestMsg, sign(attackerPem, honestMsg)));
    }

    // Replayed from an earlier connection: the nonce is fresh every time.
    {
        const QString f = fingerprintOf(0x90);
        const QByteArray sig = sign(pem, hostDigestInput("host-1", "key-1", "nonce-A", f));
        CHECK(!verify(spki, hostDigestInput("host-1", "key-1", "nonce-B", f), sig));
    }

    // Captured from a different installation and offered here.
    {
        const QString f = fingerprintOf(0xB0);
        const QByteArray sig = sign(pem, hostDigestInput("host-OTHER", "key-1", "n", f));
        CHECK(!verify(spki, hostDigestInput("host-1", "key-1", "n", f), sig));
    }

    // Domain separation: a browser signature must never pass as a host one.
    {
        const QString f = fingerprintOf(0xC0);
        const QByteArray sig = sign(pem, browserDigestInput("host-1", "n", f, f));
        CHECK(!verify(spki, hostDigestInput("host-1", "key-1", "n", f), sig));
    }

    // The browser key id inside the host digest, added at review: a signature
    // issued for one browser must not serve another.
    {
        const QString f = fingerprintOf(0xD0);
        const QByteArray sig = sign(pem, hostDigestInput("host-1", "key-THEM", "n", f));
        CHECK(!verify(spki, hostDigestInput("host-1", "key-US", "n", f), sig));
    }

    SECTION("MW-BIND — fail closed on absent or malformed signatures");

    // An absent signature is a failure, never a skip.
    CHECK(!verify(spki, honestMsg, QByteArray()));
    CHECK(!verify(spki, honestMsg, QByteArray(SIGNATURE_BYTES, '\0')));
    CHECK(!verify(spki, honestMsg, sign(pem, honestMsg).left(32)));
    // An unusable key must return false, not throw and not pass.
    CHECK(!verify(QByteArray("not a key"), honestMsg, sign(pem, honestMsg)));
    CHECK(!verify(spki, QByteArray(), sign(pem, honestMsg)));

    SECTION("MW-BIND — nonces");

    {
        QSet<QByteArray> seen;
        bool allFreshAndLongEnough = true;
        for (int i = 0; i < 50; ++i) {
            const QByteArray n = generateNonce();
            if (n.size() < MIN_NONCE_BYTES || seen.contains(n)) allFreshAndLongEnough = false;
            seen.insert(n);
        }
        CHECK(allFreshAndLongEnough);
    }
}
