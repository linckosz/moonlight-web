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

#include "PinBinding.h"

#include "PairingCrypto.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QtEndian>

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>

#include <memory>
#include <utility>

namespace {

struct GroupDeleter
{
    void operator()(EC_GROUP* g) const { EC_GROUP_free(g); }
};
struct PointDeleter
{
    void operator()(EC_POINT* p) const { EC_POINT_free(p); }
};
struct BnDeleter
{
    void operator()(BIGNUM* b) const { BN_clear_free(b); }
};
struct CtxDeleter
{
    void operator()(BN_CTX* c) const { BN_CTX_free(c); }
};

using GroupPtr = std::unique_ptr<EC_GROUP, GroupDeleter>;
using PointPtr = std::unique_ptr<EC_POINT, PointDeleter>;
using BnPtr = std::unique_ptr<BIGNUM, BnDeleter>;
using CtxPtr = std::unique_ptr<BN_CTX, CtxDeleter>;

/// Whatever the answer, the thread's OpenSSL error queue is left empty for the
/// next caller on it: a point refused off the wire leaves its reasons there.
struct OpenSslErrorsCleared
{
    ~OpenSslErrorsCleared() { ERR_clear_error(); }
};

constexpr int SCALAR_BYTES = 32;
constexpr int ELEMENT_BYTES = 65;
constexpr int HALF = 16;

// RFC 9382 §6, P-256, compressed SEC1: points nobody knows the discrete log of.
constexpr char M_HEX[] = "02886e2f97ace46e55ba9dd7242579f2993b64e16ef3dcab95afd497333d8fa12f";
constexpr char N_HEX[] = "03d8bbd6c639c62937b04d997f38c3770719c629d7014d49a24b4f98baa1292b49";

GroupPtr p256()
{
    return GroupPtr(EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1));
}

/// A point of the group from its SEC1 encoding; null for anything else, the
/// identity included.
PointPtr pointFrom(const EC_GROUP* group, const QByteArray& octets, BN_CTX* ctx)
{
    PointPtr p(EC_POINT_new(group));
    if (!p) return nullptr;
    const auto* in = reinterpret_cast<const unsigned char*>(octets.constData());
    if (EC_POINT_oct2point(group, p.get(), in, static_cast<size_t>(octets.size()), ctx) != 1)
        return nullptr;
    if (EC_POINT_is_at_infinity(group, p.get()) || EC_POINT_is_on_curve(group, p.get(), ctx) != 1)
        return nullptr;
    return p;
}

/// An element received from the peer: uncompressed only, then a point of the
/// group. RFC 9382 §7: anything else ends the run.
PointPtr elementFrom(const EC_GROUP* group, const QByteArray& octets, BN_CTX* ctx)
{
    if (octets.size() != ELEMENT_BYTES || static_cast<unsigned char>(octets.at(0)) != 0x04)
        return nullptr;
    return pointFrom(group, octets, ctx);
}

QByteArray octetsOf(const EC_GROUP* group, const EC_POINT* p, BN_CTX* ctx)
{
    QByteArray out(ELEMENT_BYTES, Qt::Uninitialized);
    auto* buf = reinterpret_cast<unsigned char*>(out.data());
    if (EC_POINT_point2oct(group, p, POINT_CONVERSION_UNCOMPRESSED, buf, ELEMENT_BYTES, ctx) !=
        static_cast<size_t>(ELEMENT_BYTES))
        return {};
    return out;
}

BnPtr bnFrom(const QByteArray& bigEndian)
{
    const auto* in = reinterpret_cast<const unsigned char*>(bigEndian.constData());
    return BnPtr(BN_bin2bn(in, static_cast<int>(bigEndian.size()), nullptr));
}

QByteArray bytesOf(const BIGNUM* bn)
{
    QByteArray out(SCALAR_BYTES, Qt::Uninitialized);
    if (BN_bn2binpad(bn, reinterpret_cast<unsigned char*>(out.data()), SCALAR_BYTES) !=
        SCALAR_BYTES)
        return {};
    return out;
}

/// The scalar a share is made with: the one given (the RFC's vectors), else
/// uniform in [1, order) from the private CSPRNG.
BnPtr scalarFor(const QByteArray& given, const BIGNUM* order)
{
    if (!given.isEmpty()) {
        BnPtr k = bnFrom(given);
        if (!k || BN_is_zero(k.get()) || BN_cmp(k.get(), order) >= 0) return nullptr;
        return k;
    }
    BnPtr k(BN_new());
    if (!k) return nullptr;
    do {
        if (BN_priv_rand_range(k.get(), order) != 1) return nullptr;
    } while (BN_is_zero(k.get()));
    return k;
}

/// scalar·P + w·Q, as two single multiplications and an addition: OpenSSL takes
/// its constant-time ladder for one scalar and one point, and not for the
/// combined form, which is meant for public scalars.
std::optional<PinBinding::Share> share(const QByteArray& w, const QByteArray& given,
                                       const char* constantHex)
{
    const OpenSslErrorsCleared clearErrors;
    GroupPtr group = p256();
    CtxPtr ctx(BN_CTX_new());
    if (!group || !ctx) return std::nullopt;
    BnPtr k = scalarFor(given, EC_GROUP_get0_order(group.get()));
    BnPtr wn = bnFrom(w);
    PointPtr q = pointFrom(group.get(), QByteArray::fromHex(constantHex), ctx.get());
    PointPtr kp(EC_POINT_new(group.get()));
    PointPtr wq(EC_POINT_new(group.get()));
    if (!k || !wn || !q || !kp || !wq) return std::nullopt;
    if (EC_POINT_mul(group.get(), kp.get(), k.get(), nullptr, nullptr, ctx.get()) != 1)
        return std::nullopt;
    if (EC_POINT_mul(group.get(), wq.get(), nullptr, q.get(), wn.get(), ctx.get()) != 1)
        return std::nullopt;
    if (EC_POINT_add(group.get(), kp.get(), kp.get(), wq.get(), ctx.get()) != 1)
        return std::nullopt;
    if (EC_POINT_is_at_infinity(group.get(), kp.get())) return std::nullopt;
    PinBinding::Share out{bytesOf(k.get()), octetsOf(group.get(), kp.get(), ctx.get())};
    if (out.scalar.isEmpty() || out.element.isEmpty()) return std::nullopt;
    return out;
}

/// K = scalar·(peer − w·Q), computed as scalar·peer − ((scalar·w) mod n)·Q: two
/// single-scalar multiplications on OpenSSL's constant-time path, and here w
/// never meets a point without the run's fresh scalar, so the one addition left
/// says nothing about w that another run would repeat. (The share's w·Q, in
/// share(), is the same in every run under one PIN: the RFC's own step.) The
/// cofactor h is 1 on P-256.
QByteArray sharedElement(const QByteArray& w, const QByteArray& scalar, const QByteArray& peer,
                         const char* constantHex)
{
    const OpenSslErrorsCleared clearErrors;
    GroupPtr group = p256();
    CtxPtr ctx(BN_CTX_new());
    if (!group || !ctx) return {};
    PointPtr theirs = elementFrom(group.get(), peer, ctx.get());
    PointPtr q = pointFrom(group.get(), QByteArray::fromHex(constantHex), ctx.get());
    BnPtr wn = bnFrom(w);
    BnPtr k = bnFrom(scalar);
    BnPtr kw(BN_new());
    PointPtr shared(EC_POINT_new(group.get()));
    PointPtr kwQ(EC_POINT_new(group.get()));
    if (!theirs || !q || !wn || !k || !kw || !shared || !kwQ) return {};
    if (BN_mod_mul(kw.get(), k.get(), wn.get(), EC_GROUP_get0_order(group.get()), ctx.get()) != 1)
        return {};
    if (EC_POINT_mul(group.get(), shared.get(), nullptr, theirs.get(), k.get(), ctx.get()) != 1)
        return {};
    if (EC_POINT_mul(group.get(), kwQ.get(), nullptr, q.get(), kw.get(), ctx.get()) != 1) return {};
    if (EC_POINT_invert(group.get(), kwQ.get(), ctx.get()) != 1) return {};
    if (EC_POINT_add(group.get(), shared.get(), shared.get(), kwQ.get(), ctx.get()) != 1) return {};
    if (EC_POINT_is_at_infinity(group.get(), shared.get())) return {};
    return octetsOf(group.get(), shared.get(), ctx.get());
}

/// RFC 9382's len(x) || x: eight bytes, little-endian.
void appendField(QByteArray& out, const QByteArray& field)
{
    char length[8];
    qToLittleEndian<quint64>(static_cast<quint64>(field.size()), length);
    out.append(length, 8);
    out.append(field);
}

QByteArray hmacSha256(const QByteArray& key, const QByteArray& message)
{
    return QMessageAuthenticationCode::hash(message, key, QCryptographicHash::Sha256);
}

/// HKDF-SHA256 (RFC 5869) with no salt, for one block of output: KcA || KcB.
QByteArray hkdfOneBlock(const QByteArray& ikm, const QByteArray& info)
{
    const QByteArray prk = hmacSha256(QByteArray(2 * HALF, '\0'), ikm);
    return hmacSha256(prk, info + QByteArray(1, '\x01'));
}

PinBinding::Keys keysOf(const QByteArray& idA, const QByteArray& idB, const QByteArray& pA,
                        const QByteArray& pB, const QByteArray& k, const QByteArray& w,
                        const QByteArray& aad)
{
    PinBinding::Keys keys;
    appendField(keys.transcript, idA);
    appendField(keys.transcript, idB);
    appendField(keys.transcript, pA);
    appendField(keys.transcript, pB);
    appendField(keys.transcript, k);
    appendField(keys.transcript, w);
    const QByteArray hash = QCryptographicHash::hash(keys.transcript, QCryptographicHash::Sha256);
    keys.ke = hash.left(HALF);
    keys.ka = hash.mid(HALF);
    const QByteArray kc = hkdfOneBlock(keys.ka, QByteArrayLiteral("ConfirmationKeys") + aad);
    keys.kcA = kc.left(HALF);
    keys.kcB = kc.mid(HALF);
    keys.confirmA = hmacSha256(keys.kcA, keys.transcript);
    keys.confirmB = hmacSha256(keys.kcB, keys.transcript);
    return keys;
}

} // namespace

namespace PinBinding {

QByteArray pinScalar(const QString& pin)
{
    const QByteArray framed =
        PairingCrypto::buildSignedMessage({QByteArray(PROTOCOL) + "|w", pin.toUtf8()});
    const QByteArray digest = QCryptographicHash::hash(framed, QCryptographicHash::Sha512);
    const OpenSslErrorsCleared clearErrors;
    GroupPtr group = p256();
    CtxPtr ctx(BN_CTX_new());
    BnPtr h = bnFrom(digest);
    BnPtr w(BN_new());
    if (!group || !ctx || !h || !w) return {};
    if (BN_nnmod(w.get(), h.get(), EC_GROUP_get0_order(group.get()), ctx.get()) != 1) return {};
    return bytesOf(w.get());
}

QByteArray tlsChannel(const QByteArray& tlsSpki)
{
    if (tlsSpki.isEmpty()) return {};
    const QByteArray digest = QCryptographicHash::hash(tlsSpki, QCryptographicHash::Sha256);
    return PairingCrypto::buildSignedMessage({QByteArrayLiteral("tls"), digest});
}

QByteArray lineChannel(const QByteArray& hostSpki)
{
    if (hostSpki.isEmpty()) return {};
    const QByteArray digest = QCryptographicHash::hash(hostSpki, QCryptographicHash::Sha256);
    return PairingCrypto::buildSignedMessage({QByteArrayLiteral("line"), digest});
}

QByteArray associatedData(const QByteArray& channel)
{
    return PairingCrypto::buildSignedMessage({QByteArray(PROTOCOL), channel});
}

std::optional<Share> shareA(const QByteArray& w, const QByteArray& x)
{
    return share(w, x, M_HEX);
}

std::optional<Share> shareB(const QByteArray& w, const QByteArray& y)
{
    return share(w, y, N_HEX);
}

std::optional<Keys> finishA(const QByteArray& w, const Share& a, const QByteArray& pB,
                            const QByteArray& idA, const QByteArray& idB, const QByteArray& aad)
{
    const QByteArray k = sharedElement(w, a.scalar, pB, N_HEX);
    if (k.isEmpty() || w.size() != SCALAR_BYTES) return std::nullopt;
    return keysOf(idA, idB, a.element, pB, k, w, aad);
}

std::optional<Keys> finishB(const QByteArray& w, const Share& b, const QByteArray& pA,
                            const QByteArray& idA, const QByteArray& idB, const QByteArray& aad)
{
    const QByteArray k = sharedElement(w, b.scalar, pA, M_HEX);
    if (k.isEmpty() || w.size() != SCALAR_BYTES) return std::nullopt;
    return keysOf(idA, idB, pA, b.element, k, w, aad);
}

bool isElement(const QByteArray& element)
{
    const OpenSslErrorsCleared clearErrors;
    GroupPtr group = p256();
    CtxPtr ctx(BN_CTX_new());
    return group && ctx && elementFrom(group.get(), element, ctx.get()) != nullptr;
}

bool sameBytes(const QByteArray& a, const QByteArray& b)
{
    // Lengths are not secret: inputs of different lengths differ, and only inputs
    // of the same length are compared, byte by byte, in constant time.
    if (a.size() != b.size()) return false;
    quint8 diff = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        diff |= static_cast<quint8>(static_cast<quint8>(a.at(i)) ^ static_cast<quint8>(b.at(i)));
    return diff == 0;
}

// ── CallBudget ─────────────────────────────────────────────────────────────

bool CallBudget::spend(const QString& bucket, qint64 nowSecs)
{
    // The callers of past windows forgotten, in one sweep per window at most: a
    // flood of new callers costs no sweep of its own.
    if (nowSecs - m_SweptAt >= m_WindowSecs) {
        for (auto it = m_Windows.begin(); it != m_Windows.end();) {
            if (nowSecs - it->start >= m_WindowSecs)
                it = m_Windows.erase(it);
            else
                ++it;
        }
        m_SweptAt = nowSecs;
    }
    auto it = m_Windows.find(bucket);
    if (it == m_Windows.end()) {
        if (m_Windows.size() >= MAX_CALLERS) return false;
        it = m_Windows.insert(bucket, Window{nowSecs, 0});
    } else if (nowSecs - it->start >= m_WindowSecs) {
        *it = Window{nowSecs, 0};
    }
    if (it->used >= m_PerWindow) return false;
    ++it->used;
    return true;
}

// ── Exchanges ──────────────────────────────────────────────────────────────

bool Exchanges::hasRoom(const QString& bucket, qint64 nowSecs)
{
    dropExpired(nowSecs);
    return m_Runs.size() < MAX_RUNS || runsOf(bucket) >= MAX_RUNS_PER_BUCKET;
}

QString Exchanges::add(Pending run, qint64 nowSecs)
{
    if (!hasRoom(run.bucket, nowSecs)) return {};
    if (runsOf(run.bucket) >= MAX_RUNS_PER_BUCKET) dropOldestOf(run.bucket);

    QByteArray raw(16, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(raw.data()), raw.size()) != 1) return {};
    const QString id = QString::fromLatin1(
        raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    run.expiresAt = nowSecs + RUN_TTL_SECS;
    m_Runs.insert(id, Kept{std::move(run), ++m_Next});
    return id;
}

std::optional<Exchanges::Pending> Exchanges::take(const QString& id, const QString& bucket,
                                                  qint64 nowSecs)
{
    dropExpired(nowSecs);
    auto it = m_Runs.find(id);
    if (it == m_Runs.end() || it->run.bucket != bucket) return std::nullopt;
    Pending run = it->run;
    m_Runs.erase(it);
    return run;
}

void Exchanges::dropExpired(qint64 nowSecs)
{
    for (auto it = m_Runs.begin(); it != m_Runs.end();) {
        if (it->run.expiresAt <= nowSecs)
            it = m_Runs.erase(it);
        else
            ++it;
    }
}

int Exchanges::runsOf(const QString& bucket) const
{
    int n = 0;
    for (auto it = m_Runs.cbegin(); it != m_Runs.cend(); ++it)
        if (it->run.bucket == bucket) ++n;
    return n;
}

void Exchanges::dropOldestOf(const QString& bucket)
{
    auto oldest = m_Runs.end();
    for (auto it = m_Runs.begin(); it != m_Runs.end(); ++it) {
        if (it->run.bucket != bucket) continue;
        if (oldest == m_Runs.end() || it->order < oldest->order) oldest = it;
    }
    if (oldest != m_Runs.end()) m_Runs.erase(oldest);
}

} // namespace PinBinding
