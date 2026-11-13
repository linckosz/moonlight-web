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

#include "LanHttpPairing.h"
#include "PhonePairing.h"

#include <QCryptographicHash>
#include <QRandomGenerator>

#include <algorithm>

namespace {

/// Crockford's base32 alphabet, as MW-PAIR-v1's codes: nothing to misread.
constexpr char kCodeAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/// Bytes of a request id: 96 bits, 16 characters of base64url, the shape
/// PhonePairing::isRequestId accepts.
constexpr int kIdBytes = 12;

QByteArray systemBytes(int n)
{
    QByteArray out;
    out.reserve(n);
    while (out.size() < n) {
        const quint32 v = QRandomGenerator::system()->generate();
        out.append(reinterpret_cast<const char*>(&v), std::min<int>(4, n - int(out.size())));
    }
    return out;
}

QString base64url(const QByteArray& bytes)
{
    return QString::fromLatin1(
        bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QByteArray claimHash(const QString& claim)
{
    return QCryptographicHash::hash(claim.toLatin1(), QCryptographicHash::Sha256);
}

/// Every byte compared, the match or not, so the time taken says nothing about
/// how much of a guess was right.
bool sameBytes(const QByteArray& a, const QByteArray& b)
{
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

} // namespace

LanHttpPairing::LanHttpPairing(RandomBytes random)
    : m_Random(random ? random : &systemBytes)
{}

void LanHttpPairing::dropExpired(qint64 now)
{
    m_Requests.removeIf([now](const Request& r) { return expired(r, now); });
}

int LanHttpPairing::indexOf(const QString& id) const
{
    for (int i = 0; i < m_Requests.size(); ++i)
        if (m_Requests[i].id == id) return i;
    return -1;
}

LanHttpPairing::OpenResult LanHttpPairing::open(const QString& address, qint64 now)
{
    OpenResult result;
    dropExpired(now);
    // A page reloaded asks again: its own earlier request makes room, whatever
    // became of it.
    m_Requests.removeIf([&address](const Request& r) { return r.address == address; });
    if (m_Requests.size() >= MAX_PENDING) {
        result.outcome = Open::Full;
        return result;
    }

    const QByteArray id = m_Random(kIdBytes);
    const QByteArray codeBytes = m_Random(CODE_LENGTH);
    const QByteArray claim = m_Random(CLAIM_BYTES);
    if (id.size() != kIdBytes || codeBytes.size() != CODE_LENGTH || claim.size() != CLAIM_BYTES) {
        result.outcome = Open::BadSource;
        return result;
    }

    Request r;
    r.id = base64url(id);
    r.address = address;
    // 256 is a multiple of 32: the low five bits of a random byte are evenly
    // spread over the alphabet.
    for (const char b : codeBytes)
        r.code.append(QLatin1Char(kCodeAlphabet[static_cast<unsigned char>(b) & 31]));
    const QString claimText = base64url(claim);
    r.claimHash = claimHash(claimText);
    r.createdAt = now;
    m_Requests.append(r);

    result.outcome = Open::Opened;
    result.request = Opened{r.id, r.code, claimText};
    return result;
}

LanHttpPairing::PollResult LanHttpPairing::poll(const QString& id, const QString& claim,
                                                const QString& address, qint64 now)
{
    PollResult result;
    dropExpired(now);
    const int i = indexOf(id);
    if (i < 0) return result;
    const Request& r = m_Requests[i];
    // Another address, or not the page that asked: the same answer as for a
    // request that never was.
    const bool claimOk = sameBytes(r.claimHash, claimHash(claim));
    if (r.address != address || !claimOk) return result;

    switch (r.state) {
    case State::Pending: result.state = Poll::Pending; return result;
    case State::Approved:
        result.state = Poll::Approved;
        result.approverId = r.approverId;
        break;
    case State::Refused: result.state = Poll::Refused; break;
    }
    m_Requests.removeAt(i);
    return result;
}

std::optional<LanHttpPairing::Shown> LanHttpPairing::find(const QString& id, qint64 now) const
{
    const int i = indexOf(id);
    if (i < 0) return std::nullopt;
    const Request& r = m_Requests[i];
    if (expired(r, now) || r.state != State::Pending) return std::nullopt;
    return Shown{r.address, TTL_SECS - (now - r.createdAt)};
}

LanHttpPairing::Answer LanHttpPairing::approve(const QString& id, const QString& typedCode,
                                               const QString& approverId, qint64 now)
{
    dropExpired(now);
    const int i = indexOf(id);
    if (i < 0 || m_Requests[i].state != State::Pending) return Answer::Gone;
    Request& r = m_Requests[i];
    if (!sameBytes(PhonePairing::normalizeCode(typedCode).toLatin1(), r.code.toLatin1())) {
        if (++r.tries >= CODE_TRIES) {
            m_Requests.removeAt(i);
            return Answer::Gone;
        }
        return Answer::WrongCode;
    }
    r.state = State::Approved;
    r.approverId = approverId;
    return Answer::Approved;
}

bool LanHttpPairing::refuse(const QString& id, qint64 now)
{
    dropExpired(now);
    const int i = indexOf(id);
    if (i < 0 || m_Requests[i].state != State::Pending) return false;
    m_Requests[i].state = State::Refused;
    return true;
}

int LanHttpPairing::pending(qint64 now) const
{
    return static_cast<int>(
        std::count_if(m_Requests.cbegin(), m_Requests.cend(), [now](const Request& r) {
            return !expired(r, now) && r.state == State::Pending;
        }));
}
