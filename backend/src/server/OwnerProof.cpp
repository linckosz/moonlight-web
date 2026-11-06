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

#include "server/OwnerProof.h"

#include "common/PairingCrypto.h"

QByteArray OwnerProof::issue(const QString& session, qint64 nowMs)
{
    if (session.isEmpty()) return {};
    prune(nowMs);

    int held = 0;
    int oldest = -1;
    for (int i = 0; i < m_Pending.size(); ++i) {
        if (m_Pending.at(i).session != session) continue;
        if (oldest < 0) oldest = i;
        ++held;
    }
    if (held >= kPerSession) {
        m_Pending.removeAt(oldest);
    } else if (m_Pending.size() >= kTotal) {
        return {};
    }

    Pending p;
    p.session = session;
    p.nonce = PairingCrypto::generateNonce();
    p.expiresMs = nowMs + kLifetimeMs;
    if (p.nonce.size() < PairingCrypto::MIN_NONCE_BYTES) return {};
    m_Pending.append(p);
    return p.nonce;
}

QByteArray OwnerProof::message(const QString& hostId, const QByteArray& nonce,
                               const QString& action, int slot)
{
    return PairingCrypto::buildSignedMessage(
        {QByteArray(kLabel), hostId.toUtf8(), nonce, action.toUtf8(), QByteArray::number(slot)});
}

OwnerProof::Result OwnerProof::check(const QString& session, const QByteArray& spki,
                                     const QString& hostId, const QString& action, int slot,
                                     const QJsonObject& proof, qint64 nowMs)
{
    const QString nonceB64 = proof.value(QStringLiteral("nonce")).toString();
    const QString sigB64 = proof.value(QStringLiteral("sig")).toString();
    if (nonceB64.isEmpty() || sigB64.isEmpty()) return Result::Missing;

    prune(nowMs);
    const QByteArray nonce = QByteArray::fromBase64(nonceB64.toLatin1());
    int found = -1;
    for (int i = 0; i < m_Pending.size(); ++i) {
        const Pending& p = m_Pending.at(i);
        if (p.session == session && p.nonce == nonce) {
            found = i;
            break;
        }
    }
    if (found < 0 || session.isEmpty() || nonce.isEmpty()) return Result::Refused;
    m_Pending.removeAt(found);

    const QByteArray sig = QByteArray::fromBase64(sigB64.toLatin1());
    return PairingCrypto::verify(spki, message(hostId, nonce, action, slot), sig) ? Result::Ok
                                                                                  : Result::Refused;
}

void OwnerProof::prune(qint64 nowMs)
{
    for (int i = m_Pending.size() - 1; i >= 0; --i)
        if (m_Pending.at(i).expiresMs <= nowMs) m_Pending.removeAt(i);
}
