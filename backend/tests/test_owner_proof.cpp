/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The sharing board's proof that it is the browser its session was paired with
 * (OwnerProof): a signature by the session's pairing key over a challenge the
 * host drew for that session, single use, short-lived, and good for one action
 * on one row only.
 */
#include "test_framework.h"

#include "common/PairingCrypto.h"
#include "server/OwnerProof.h"

#include <QJsonObject>
#include <QString>

namespace {

const QString kHostId = QStringLiteral("host-key-id");

struct Browser
{
    QByteArray pem;
    QByteArray spki;
};

Browser newBrowser()
{
    Browser b;
    b.pem = PairingCrypto::generatePrivateKeyPem();
    b.spki = PairingCrypto::publicKeySpkiFromPrivatePem(b.pem);
    return b;
}

QJsonObject proofFor(const Browser& b, const QByteArray& nonce, const QString& action, int slot,
                     const QString& hostId = kHostId)
{
    const QByteArray sig =
        PairingCrypto::sign(b.pem, OwnerProof::message(hostId, nonce, action, slot));
    QJsonObject p;
    p[QStringLiteral("nonce")] = QString::fromLatin1(nonce.toBase64());
    p[QStringLiteral("sig")] = QString::fromLatin1(sig.toBase64());
    return p;
}

} // namespace

void run_owner_proof_tests()
{
    using R = OwnerProof::Result;
    const qint64 t0 = 1'000'000;

    SECTION("owner proof — the signed bytes, as the page builds them");
    // The same vector is pinned in frontend/test/shareOwnerProof.test.js.
    CHECK_EQ(
        QString::fromLatin1(OwnerProof::message(QStringLiteral("H"), QByteArray("\x01\x02\x03", 3),
                                                QStringLiteral("activate"), 2)
                                .toHex()),
        QStringLiteral("00000016"
                       "4d572d42494e442d76317c73686172652d6f776e6572"
                       "0000000148"
                       "00000003010203"
                       "000000086163746976617465"
                       "0000000132"));

    SECTION("owner proof — the paired browser's signature opens the row, once");
    {
        OwnerProof proofs;
        const Browser b = newBrowser();
        CHECK(!b.spki.isEmpty());
        const QByteArray nonce = proofs.issue(QStringLiteral("S"), t0);
        CHECK_EQ(nonce.size(), 32);
        const QJsonObject p = proofFor(b, nonce, QStringLiteral("activate"), 2);
        CHECK(proofs.check(QStringLiteral("S"), b.spki, kHostId, QStringLiteral("activate"), 2, p,
                           t0 + 10) == R::Ok);
        // Spent: the same proof never opens it again.
        CHECK(proofs.check(QStringLiteral("S"), b.spki, kHostId, QStringLiteral("activate"), 2, p,
                           t0 + 20) == R::Refused);
        CHECK_EQ(proofs.pendingCount(), 0);
    }

    SECTION("owner proof — another key, session, action, row or host is refused");
    {
        OwnerProof proofs;
        const Browser b = newBrowser();
        const Browser other = newBrowser();
        const QString s = QStringLiteral("S");
        const QString act = QStringLiteral("credentials");
        auto fresh = [&]() { return proofs.issue(s, t0); };

        // Signed by a key the session was not paired with.
        CHECK(proofs.check(s, b.spki, kHostId, act, 3, proofFor(other, fresh(), act, 3), t0) ==
              R::Refused);
        // The challenge of another session.
        {
            const QByteArray n = proofs.issue(QStringLiteral("T"), t0);
            CHECK(proofs.check(s, b.spki, kHostId, act, 3, proofFor(b, n, act, 3), t0) ==
                  R::Refused);
        }
        // Signed for another action, another row, another host.
        CHECK(proofs.check(s, b.spki, kHostId, act, 3,
                           proofFor(b, fresh(), QStringLiteral("activate"), 3), t0) == R::Refused);
        CHECK(proofs.check(s, b.spki, kHostId, act, 3, proofFor(b, fresh(), act, 4), t0) ==
              R::Refused);
        CHECK(proofs.check(s, b.spki, kHostId, act, 3,
                           proofFor(b, fresh(), act, 3, QStringLiteral("elsewhere")),
                           t0) == R::Refused);
        // A nonce the host never drew.
        CHECK(proofs.check(s, b.spki, kHostId, act, 3,
                           proofFor(b, PairingCrypto::generateNonce(), act, 3), t0) == R::Refused);
        // A signature with one byte changed.
        {
            QJsonObject p = proofFor(b, fresh(), act, 3);
            QByteArray sig =
                QByteArray::fromBase64(p.value(QStringLiteral("sig")).toString().toLatin1());
            sig[5] = static_cast<char>(sig[5] ^ 0x01);
            p[QStringLiteral("sig")] = QString::fromLatin1(sig.toBase64());
            CHECK(proofs.check(s, b.spki, kHostId, act, 3, p, t0) == R::Refused);
        }
        // No session at all.
        CHECK(proofs.issue(QString(), t0).isEmpty());
        CHECK(proofs.check(QString(), b.spki, kHostId, act, 3, proofFor(b, fresh(), act, 3), t0) ==
              R::Refused);
        // The right one still works after all that.
        CHECK(proofs.check(s, b.spki, kHostId, act, 3, proofFor(b, fresh(), act, 3), t0) == R::Ok);
    }

    SECTION("owner proof — a request without one is told so, not counted");
    {
        OwnerProof proofs;
        const Browser b = newBrowser();
        const QString s = QStringLiteral("S");
        CHECK(proofs.check(s, b.spki, kHostId, QStringLiteral("activate"), 2, QJsonObject(), t0) ==
              R::Missing);
        QJsonObject half;
        half[QStringLiteral("nonce")] = QString::fromLatin1(proofs.issue(s, t0).toBase64());
        CHECK(proofs.check(s, b.spki, kHostId, QStringLiteral("activate"), 2, half, t0) ==
              R::Missing);
    }

    SECTION("owner proof — a challenge waits two minutes, no longer");
    {
        OwnerProof proofs;
        const Browser b = newBrowser();
        const QString s = QStringLiteral("S");
        const QString act = QStringLiteral("permissions");
        const QByteArray late = proofs.issue(s, t0);
        CHECK(proofs.check(s, b.spki, kHostId, act, 2, proofFor(b, late, act, 2),
                           t0 + OwnerProof::kLifetimeMs) == R::Refused);
        const QByteArray onTime = proofs.issue(s, t0);
        CHECK(proofs.check(s, b.spki, kHostId, act, 2, proofFor(b, onTime, act, 2),
                           t0 + OwnerProof::kLifetimeMs - 1) == R::Ok);
    }

    SECTION("owner proof — one session's challenges never push out another's");
    {
        OwnerProof proofs;
        const Browser b = newBrowser();
        const QString act = QStringLiteral("activate");

        // A session past its share retires its own oldest.
        const QByteArray first = proofs.issue(QStringLiteral("S"), t0);
        QByteArray last;
        for (int i = 0; i < OwnerProof::kPerSession; ++i)
            last = proofs.issue(QStringLiteral("S"), t0);
        CHECK_EQ(proofs.pendingCount(), OwnerProof::kPerSession);
        CHECK(proofs.check(QStringLiteral("S"), b.spki, kHostId, act, 2, proofFor(b, first, act, 2),
                           t0) == R::Refused);
        CHECK(proofs.check(QStringLiteral("S"), b.spki, kHostId, act, 2, proofFor(b, last, act, 2),
                           t0) == R::Ok);

        // The store full: a newcomer is refused, nobody is evicted for it.
        OwnerProof full;
        const QByteArray keep = full.issue(QStringLiteral("victim"), t0);
        for (int i = 1; i < OwnerProof::kTotal; ++i)
            CHECK(!full.issue(QStringLiteral("s%1").arg(i), t0).isEmpty());
        CHECK_EQ(full.pendingCount(), OwnerProof::kTotal);
        CHECK(full.issue(QStringLiteral("newcomer"), t0).isEmpty());
        CHECK(full.check(QStringLiteral("victim"), b.spki, kHostId, act, 2,
                         proofFor(b, keep, act, 2), t0) == R::Ok);
        // Room again once they expire.
        CHECK(!full.issue(QStringLiteral("newcomer"), t0 + OwnerProof::kLifetimeMs).isEmpty());
    }
}
