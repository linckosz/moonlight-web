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

#include "server/PhonePairing.h"

#include "common/PairingCrypto.h"

#include <QCryptographicHash>
#include <QList>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <utility>

namespace {

/// Crockford's base32 alphabet: no I, L, O or U, nothing to misread on a TV.
constexpr char kCodeAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/// Bytes of a request id: 96 bits, 16 characters of base64url.
constexpr int kIdBytes = 12;

QByteArray label(const char* what)
{
    return QByteArray(PhonePairing::PROTOCOL) + '|' + what;
}

QByteArray sha256(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

/// A fresh id from OpenSSL's generator (PairingCrypto::generateNonce), empty
/// when it has none to give.
QString newRequestId()
{
    const QByteArray random = PairingCrypto::generateNonce().left(kIdBytes);
    if (random.size() != kIdBytes) return {};
    return QString::fromLatin1(
        random.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

} // namespace

QByteArray PhonePairing::commitment(const QByteArray& nonce)
{
    return sha256(PairingCrypto::buildSignedMessage({label("commit"), nonce}));
}

QString PhonePairing::code(const QByteArray& hostKey, const QByteArray& deviceKey,
                           const QString& id, const QByteArray& nonce)
{
    const QByteArray digest = sha256(
        PairingCrypto::buildSignedMessage({label("code"), hostKey, deviceKey, id.toUtf8(), nonce}));
    const quint32 bits = (quint32(quint8(digest[0])) << 16) | (quint32(quint8(digest[1])) << 8) |
                         quint32(quint8(digest[2]));
    QString out;
    for (int i = 0; i < CODE_LENGTH; ++i)
        out.append(QLatin1Char(kCodeAlphabet[(bits >> (19 - 5 * i)) & 31]));
    return out;
}

QByteArray PhonePairing::claimMessage(const QString& hostId, const QString& id,
                                      const QByteArray& nonce)
{
    return PairingCrypto::buildSignedMessage({label("claim"), hostId.toUtf8(), id.toUtf8(), nonce});
}

QByteArray PhonePairing::approveMessage(const QString& hostId, const QString& id,
                                        const QString& code)
{
    return PairingCrypto::buildSignedMessage(
        {label("approve"), hostId.toUtf8(), id.toUtf8(), code.toUtf8()});
}

QByteArray PhonePairing::refuseMessage(const QString& hostId, const QString& id)
{
    return PairingCrypto::buildSignedMessage({label("refuse"), hostId.toUtf8(), id.toUtf8()});
}

QString PhonePairing::normalizeCode(const QString& typed)
{
    QString out;
    for (const QChar c : typed) {
        if (c.isSpace() || c == QLatin1Char('-')) continue;
        const QChar upper = c.toUpper();
        if (upper == QLatin1Char('O'))
            out.append(QLatin1Char('0'));
        else if (upper == QLatin1Char('I') || upper == QLatin1Char('L'))
            out.append(QLatin1Char('1'));
        else
            out.append(upper);
    }
    return out;
}

bool PhonePairing::isRequestId(const QString& id)
{
    static const QRegularExpression kShape(QStringLiteral("^[A-Za-z0-9_-]{16,22}$"));
    return kShape.match(id).hasMatch();
}

QString PhonePairing::cleanName(const QString& name)
{
    // By code point, so that a character beyond the first plane (an emoji) is
    // kept whole rather than dropped as two halves that print nothing.
    QString out;
    out.reserve(name.size());
    for (qsizetype i = 0; i < name.size(); ++i) {
        const QChar c = name.at(i);
        if (c.isHighSurrogate() && i + 1 < name.size() && name.at(i + 1).isLowSurrogate()) {
            const QChar low = name.at(++i);
            if (QChar::isPrint(QChar::surrogateToUcs4(c, low))) {
                out.append(c);
                out.append(low);
            }
        } else if (!c.isSurrogate() && c.isPrint()) {
            out.append(c);
        }
    }
    out = out.simplified();
    // MAX_NAME at most, and never half of a pair.
    if (out.size() > MAX_NAME)
        out.truncate(out.at(MAX_NAME - 1).isHighSurrogate() ? MAX_NAME - 1 : MAX_NAME);
    return out;
}

void PhonePairing::purge(qint64 now)
{
    for (auto it = m_requests.begin(); it != m_requests.end();) {
        const bool gone = it->state == State::Paired ? now - it->pairedAt > GRACE_SECS
                                                     : now - it->createdAt > TTL_SECS + GRACE_SECS;
        if (gone)
            it = m_requests.erase(it);
        else
            ++it;
    }
}

PhonePairing::OpenResult PhonePairing::open(const QByteArray& deviceKey, const QString& machineName,
                                            const QByteArray& commitment, const QString& address,
                                            const QString& bucket, bool viaTunnel, bool fromLan,
                                            qint64 now)
{
    purge(now);
    OpenResult result;

    // The same device asks again from the same place, a new code on the same
    // screen: its older request goes. Not one asked from elsewhere: a device's
    // key is no secret, and naming it must not cancel that device's request.
    for (auto it = m_requests.begin(); it != m_requests.end();) {
        if (it->bucket == bucket && it->deviceKey == deviceKey && it->state != State::Paired)
            it = m_requests.erase(it);
        else
            ++it;
    }

    // One address holds a few at most: its oldest makes room, and the caller
    // hears of it (Open::Crowded). Oldest by birth, however many were born in
    // the same second. The ids are taken before anything is erased, so no
    // pointer outlives its entry.
    QList<const Request*> sameAddress;
    for (const Request& r : std::as_const(m_requests)) {
        if (live(r, now) && r.bucket == bucket) sameAddress.append(&r);
    }
    std::sort(sameAddress.begin(), sameAddress.end(),
              [](const Request* a, const Request* b) { return olderThan(*a, *b); });
    QStringList oldest;
    for (int i = 0; i + MAX_PER_ADDRESS - 1 < sameAddress.size(); ++i)
        oldest.append(sameAddress.at(i)->id);
    for (const QString& id : std::as_const(oldest))
        m_requests.remove(id);

    // The book's places, and among them those a request from elsewhere may
    // take: the rest stays free for this LAN's devices.
    int held = 0;
    int heldFromElsewhere = 0;
    for (const Request& r : std::as_const(m_requests)) {
        if (!live(r, now)) continue;
        ++held;
        if (!r.fromLan) ++heldFromElsewhere;
    }
    if (held >= MAX_PENDING) return result;
    if (!fromLan && heldFromElsewhere >= MAX_FROM_ELSEWHERE) return result;

    Request r;
    for (int tries = 0; tries < 4 && (r.id.isEmpty() || m_requests.contains(r.id)); ++tries)
        r.id = newRequestId();
    if (r.id.isEmpty() || m_requests.contains(r.id)) return result;
    r.deviceKey = deviceKey;
    r.machineName = cleanName(machineName);
    r.commitment = commitment;
    r.address = address;
    r.bucket = bucket;
    r.viaTunnel = viaTunnel;
    r.fromLan = fromLan;
    r.createdAt = now;
    r.sequence = ++m_births;
    m_requests.insert(r.id, r);
    result.outcome = oldest.isEmpty() ? Open::Opened : Open::Crowded;
    result.request = r;
    return result;
}

bool PhonePairing::hasFromElsewhere() const
{
    for (const Request& r : m_requests) {
        if (!r.fromLan) return true;
    }
    return false;
}

int PhonePairing::dropFromElsewhere()
{
    int dropped = 0;
    for (auto it = m_requests.begin(); it != m_requests.end();) {
        if (it->fromLan) {
            ++it;
        } else {
            it = m_requests.erase(it);
            ++dropped;
        }
    }
    return dropped;
}

PhonePairing::ClaimResult PhonePairing::claim(const QString& id, const QByteArray& nonce,
                                              const QByteArray& signature, const QString& hostId,
                                              const QByteArray& hostKey, bool viaTunnel,
                                              const QByteArray& tunnelKey, qint64 now)
{
    purge(now);
    ClaimResult result;
    auto it = m_requests.find(id);
    if (it == m_requests.end()) return result; // Unknown

    if (expired(*it, now)) {
        result.outcome = Claim::Expired;
        return result;
    }

    // Who is asking: the device, by its key, the way it asked. Over a line, by
    // the line's key as well, which the line's own MW-BIND exchange proved.
    const bool wayOk = viaTunnel == it->viaTunnel && (!viaTunnel || tunnelKey == it->deviceKey);
    if (!wayOk || nonce.size() != NONCE_BYTES ||
        !PairingCrypto::verify(it->deviceKey, claimMessage(hostId, id, nonce), signature)) {
        result.outcome = Claim::BadProof;
        return result;
    }

    // The device's own key signed this nonce. Not the one it committed to, or
    // another one than it revealed before: whatever that is, it ends here.
    if (it->nonce.isEmpty()) {
        if (commitment(nonce) != it->commitment) {
            m_requests.erase(it);
            result.outcome = Claim::BadProof;
            return result;
        }
        it->nonce = nonce;
        it->code = code(hostKey, it->deviceKey, id, nonce);
    } else if (it->nonce != nonce) {
        m_requests.erase(it);
        result.outcome = Claim::BadProof;
        return result;
    }

    result.request = *it;
    switch (it->state) {
    case State::Waiting: result.outcome = Claim::Waiting; return result;
    // Approved, or paired already and asked again: the caller makes the
    // session (in place of the earlier one) and says so with markPaired().
    case State::Approved:
    case State::Paired: result.outcome = Claim::Paired; return result;
    case State::Refused: result.outcome = Claim::Refused; break;
    }
    // Told once: a refused request is spent.
    m_requests.erase(it);
    return result;
}

void PhonePairing::markPaired(const QString& id, const QString& sessionId, qint64 now)
{
    auto it = m_requests.find(id);
    if (it == m_requests.end()) return;
    it->state = State::Paired;
    it->sessionId = sessionId;
    it->pairedAt = now;
}

void PhonePairing::drop(const QString& id)
{
    m_requests.remove(id);
}

PhonePairing::Lookup PhonePairing::find(const QString& id, qint64 now, Request* out)
{
    purge(now);
    const auto it = m_requests.constFind(id);
    if (it == m_requests.constEnd()) return Lookup::Unknown;
    if (expired(*it, now)) return Lookup::Expired;
    if (out) *out = *it;
    return Lookup::Found;
}

PhonePairing::AnswerResult PhonePairing::approve(const QString& id, const QString& typedCode,
                                                 const QString& approver, qint64 now)
{
    purge(now);
    AnswerResult result;
    auto it = m_requests.find(id);
    if (it == m_requests.end()) return result; // Unknown
    if (it->state == State::Paired) {
        result.outcome = Answer::Decided;
        return result;
    }
    if (expired(*it, now)) {
        result.outcome = Answer::Expired;
        return result;
    }
    if (it->state != State::Waiting) {
        result.outcome = Answer::Decided;
        return result;
    }
    if (it->code.isEmpty()) {
        result.outcome = Answer::NotReady;
        return result;
    }
    if (normalizeCode(typedCode) != it->code) {
        result.triesLeft = qMax(0, CODE_TRIES - ++it->wrongCodes);
        if (result.triesLeft > 0) {
            result.outcome = Answer::CodeMismatch;
            return result;
        }
        // Not the code of the device in front of the administrator: refused,
        // as if they had said no.
        it->state = State::Refused;
        result.outcome = Answer::TooManyTries;
        result.address = it->address;
        return result;
    }
    it->state = State::Approved;
    it->approver = approver;
    result.outcome = Answer::Done;
    return result;
}

PhonePairing::AnswerResult PhonePairing::refuse(const QString& id, qint64 now)
{
    purge(now);
    AnswerResult result;
    auto it = m_requests.find(id);
    if (it == m_requests.end()) return result; // Unknown
    if (it->state == State::Paired) {
        result.outcome = Answer::Decided;
        return result;
    }
    if (expired(*it, now)) {
        result.outcome = Answer::Expired;
        return result;
    }
    if (it->state != State::Waiting) {
        result.outcome = Answer::Decided;
        return result;
    }
    it->state = State::Refused;
    result.outcome = Answer::Done;
    result.address = it->address;
    return result;
}
