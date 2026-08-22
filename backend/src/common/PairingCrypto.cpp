/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "PairingCrypto.h"

#include <QCryptographicHash>
#include <QtEndian>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include <memory>

namespace {

struct PkeyDeleter
{
    void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); }
};
struct BioDeleter
{
    void operator()(BIO* b) const { BIO_free(b); }
};
struct MdCtxDeleter
{
    void operator()(EVP_MD_CTX* c) const { EVP_MD_CTX_free(c); }
};
struct SigDeleter
{
    void operator()(ECDSA_SIG* s) const { ECDSA_SIG_free(s); }
};

using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using BioPtr = std::unique_ptr<BIO, BioDeleter>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;
using SigPtr = std::unique_ptr<ECDSA_SIG, SigDeleter>;

/// Half of a P-256 raw signature: one 32-byte scalar.
constexpr int SCALAR_BYTES = PairingCrypto::SIGNATURE_BYTES / 2;

/// True when @p key is an EC key on the P-256 curve and nothing else. Callers
/// rely on this so the raw r‖s conversions below can assume 32-byte scalars.
bool isP256(EVP_PKEY* key)
{
    if (!key || EVP_PKEY_get_base_id(key) != EVP_PKEY_EC) return false;

    char group[64] = {0};
    size_t len = 0;
    if (EVP_PKEY_get_utf8_string_param(key, OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof(group),
                                       &len) != 1) {
        return false;
    }
    // OpenSSL reports the ANSI X9.62 name; WebCrypto and the RFCs say "P-256".
    return qstrcmp(group, SN_X9_62_prime256v1) == 0 || qstrcmp(group, "P-256") == 0;
}

PkeyPtr loadPrivatePem(const QByteArray& pem)
{
    if (pem.isEmpty()) return nullptr;
    BioPtr bio(BIO_new_mem_buf(pem.constData(), pem.size()));
    if (!bio) return nullptr;
    PkeyPtr key(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
    if (!isP256(key.get())) return nullptr;
    return key;
}

PkeyPtr loadPublicSpki(const QByteArray& der)
{
    if (der.isEmpty()) return nullptr;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(der.constData());
    PkeyPtr key(d2i_PUBKEY(nullptr, &p, static_cast<long>(der.size())));
    if (!isP256(key.get())) return nullptr;
    return key;
}

/// DER ECDSA signature → the raw r‖s pair WebCrypto speaks. Empty on failure.
QByteArray derToRaw(const QByteArray& der)
{
    const unsigned char* p = reinterpret_cast<const unsigned char*>(der.constData());
    SigPtr sig(d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(der.size())));
    if (!sig) return {};

    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(sig.get(), &r, &s);
    if (!r || !s) return {};

    QByteArray raw(PairingCrypto::SIGNATURE_BYTES, Qt::Uninitialized);
    auto* out = reinterpret_cast<unsigned char*>(raw.data());
    // bn2binpad left-pads, which is what fixed-width r‖s requires: a scalar
    // whose top byte happens to be zero must still occupy 32 bytes.
    if (BN_bn2binpad(r, out, SCALAR_BYTES) != SCALAR_BYTES) return {};
    if (BN_bn2binpad(s, out + SCALAR_BYTES, SCALAR_BYTES) != SCALAR_BYTES) return {};
    return raw;
}

/// Raw r‖s → DER, so OpenSSL can verify what a browser produced. Empty on
/// failure, including a signature of the wrong length.
QByteArray rawToDer(const QByteArray& raw)
{
    if (raw.size() != PairingCrypto::SIGNATURE_BYTES) return {};

    const auto* in = reinterpret_cast<const unsigned char*>(raw.constData());
    BIGNUM* r = BN_bin2bn(in, SCALAR_BYTES, nullptr);
    BIGNUM* s = BN_bin2bn(in + SCALAR_BYTES, SCALAR_BYTES, nullptr);
    if (!r || !s) {
        BN_free(r);
        BN_free(s);
        return {};
    }

    SigPtr sig(ECDSA_SIG_new());
    if (!sig) {
        BN_free(r);
        BN_free(s);
        return {};
    }
    // Takes ownership of r and s on success — do not free them past this point.
    if (ECDSA_SIG_set0(sig.get(), r, s) != 1) {
        BN_free(r);
        BN_free(s);
        return {};
    }

    unsigned char* der = nullptr;
    const int len = i2d_ECDSA_SIG(sig.get(), &der);
    if (len <= 0 || !der) return {};
    QByteArray out(reinterpret_cast<const char*>(der), len);
    OPENSSL_free(der);
    return out;
}

} // namespace

namespace PairingCrypto {

// ── Key material ───────────────────────────────────────────────────────────

QByteArray generatePrivateKeyPem()
{
    PkeyPtr key(EVP_EC_gen("P-256"));
    if (!key) return {};

    BioPtr bio(BIO_new(BIO_s_mem()));
    if (!bio) return {};
    if (PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        return {};
    }

    char* data = nullptr;
    const long len = BIO_get_mem_data(bio.get(), &data);
    if (len <= 0 || !data) return {};
    return QByteArray(data, static_cast<int>(len));
}

QByteArray publicKeySpkiFromPrivatePem(const QByteArray& privatePem)
{
    PkeyPtr key = loadPrivatePem(privatePem);
    if (!key) return {};

    unsigned char* der = nullptr;
    const int len = i2d_PUBKEY(key.get(), &der);
    if (len <= 0 || !der) return {};
    QByteArray out(reinterpret_cast<const char*>(der), len);
    OPENSSL_free(der);
    return out;
}

QString keyId(const QByteArray& spkiDer)
{
    if (spkiDer.isEmpty()) return {};
    const QByteArray digest = QCryptographicHash::hash(spkiDer, QCryptographicHash::Sha256);
    return QString::fromLatin1(
        digest.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool isValidP256Spki(const QByteArray& spkiDer)
{
    return loadPublicSpki(spkiDer) != nullptr;
}

// ── Message construction ───────────────────────────────────────────────────

QByteArray buildSignedMessage(const QVector<QByteArray>& fields)
{
    QByteArray out;
    int total = 0;
    for (const QByteArray& f : fields) total += 4 + f.size();
    out.reserve(total);

    for (const QByteArray& f : fields) {
        char prefix[4];
        qToBigEndian<quint32>(static_cast<quint32>(f.size()), prefix);
        out.append(prefix, 4);
        out.append(f);
    }
    return out;
}

QByteArray hostDigestInput(const QString& hostId, const QString& browserKeyId,
                           const QByteArray& nonceB, const QString& fingerprintHost)
{
    return buildSignedMessage({QByteArray(PROTOCOL) + "|host", hostId.toUtf8(),
                               browserKeyId.toUtf8(), nonceB, fingerprintHost.toUtf8()});
}

QByteArray browserDigestInput(const QString& hostId, const QByteArray& nonceH,
                              const QString& fingerprintHost, const QString& fingerprintBrowser)
{
    return buildSignedMessage({QByteArray(PROTOCOL) + "|browser", hostId.toUtf8(), nonceH,
                               fingerprintHost.toUtf8(), fingerprintBrowser.toUtf8()});
}

// ── Sign / verify ──────────────────────────────────────────────────────────

QByteArray sign(const QByteArray& privatePem, const QByteArray& message)
{
    PkeyPtr key = loadPrivatePem(privatePem);
    if (!key || message.isEmpty()) return {};

    MdCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx) return {};
    if (EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1) return {};

    const auto* msg = reinterpret_cast<const unsigned char*>(message.constData());
    const size_t msgLen = static_cast<size_t>(message.size());

    size_t derLen = 0;
    if (EVP_DigestSign(ctx.get(), nullptr, &derLen, msg, msgLen) != 1 || derLen == 0) return {};

    QByteArray der(static_cast<int>(derLen), Qt::Uninitialized);
    if (EVP_DigestSign(ctx.get(), reinterpret_cast<unsigned char*>(der.data()), &derLen, msg,
                       msgLen) != 1) {
        return {};
    }
    der.resize(static_cast<int>(derLen));
    return derToRaw(der);
}

bool verify(const QByteArray& spkiDer, const QByteArray& message, const QByteArray& signature)
{
    // Every rejection below returns the same false. A caller must not be able to
    // distinguish "wrong signature" from "unparseable key" and treat one as a
    // reason to continue.
    if (message.isEmpty() || signature.size() != SIGNATURE_BYTES) return false;

    PkeyPtr key = loadPublicSpki(spkiDer);
    if (!key) return false;

    const QByteArray der = rawToDer(signature);
    if (der.isEmpty()) return false;

    MdCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx) return false;
    if (EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1) {
        return false;
    }

    return EVP_DigestVerify(ctx.get(), reinterpret_cast<const unsigned char*>(der.constData()),
                            static_cast<size_t>(der.size()),
                            reinterpret_cast<const unsigned char*>(message.constData()),
                            static_cast<size_t>(message.size())) == 1;
}

// ── Nonces ─────────────────────────────────────────────────────────────────

QByteArray generateNonce()
{
    QByteArray nonce(32, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()), nonce.size()) != 1) {
        // No fallback to a weaker source: a predictable nonce would let a
        // captured signature be replayed, which is the whole point of having one.
        return {};
    }
    return nonce;
}

} // namespace PairingCrypto
