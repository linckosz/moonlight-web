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

#include <utility>

OwnerProof::OwnerProof(QByteArray label, Limits limits)
    : m_Label(std::move(label))
    , m_Limits(limits)
{}

QByteArray OwnerProof::issue(const QString& session, qint64 nowMs, const QString& scope)
{
    if (session.isEmpty()) return {};
    prune(nowMs);

    // A new challenge for one scope stands for the one before it.
    if (!scope.isEmpty())
        for (int i = m_Pending.size() - 1; i >= 0; --i)
            if (m_Pending.at(i).session == session && m_Pending.at(i).scope == scope)
                m_Pending.removeAt(i);

    int held = 0;
    int oldest = -1;
    for (int i = 0; i < m_Pending.size(); ++i) {
        if (m_Pending.at(i).session != session) continue;
        if (oldest < 0) oldest = i;
        ++held;
    }
    if (held >= m_Limits.perSession) {
        m_Pending.removeAt(oldest);
    } else if (m_Pending.size() >= m_Limits.total) {
        return {};
    }

    Pending p;
    p.session = session;
    p.scope = scope;
    p.nonce = PairingCrypto::generateNonce();
    p.expiresMs = nowMs + m_Limits.lifetimeMs;
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

QByteArray OwnerProof::messageFor(const QString& hostId, const QByteArray& nonce,
                                  const QList<QByteArray>& fields) const
{
    QList<QByteArray> all{m_Label, hostId.toUtf8(), nonce};
    all.append(fields);
    return PairingCrypto::buildSignedMessage(all);
}

OwnerProof::Result OwnerProof::take(const QString& session, const QJsonObject& proof, qint64 nowMs,
                                    QByteArray& nonce, QByteArray& sig, const QString& scope)
{
    const QString nonceB64 = proof.value(QStringLiteral("nonce")).toString();
    const QString sigB64 = proof.value(QStringLiteral("sig")).toString();
    if (nonceB64.isEmpty() || sigB64.isEmpty()) return Result::Missing;

    prune(nowMs);
    nonce = QByteArray::fromBase64(nonceB64.toLatin1());
    int found = -1;
    for (int i = 0; i < m_Pending.size(); ++i) {
        const Pending& p = m_Pending.at(i);
        if (p.session == session && p.nonce == nonce) {
            found = i;
            break;
        }
    }
    if (found < 0 || session.isEmpty() || nonce.isEmpty()) return Result::Refused;
    const bool sameScope = m_Pending.at(found).scope == scope;
    m_Pending.removeAt(found);
    if (!sameScope) return Result::Refused;

    sig = QByteArray::fromBase64(sigB64.toLatin1());
    return Result::Ok;
}

OwnerProof::Result OwnerProof::check(const QString& session, const QByteArray& spki,
                                     const QString& hostId, const QString& action, int slot,
                                     const QJsonObject& proof, qint64 nowMs)
{
    QByteArray nonce;
    QByteArray sig;
    const Result taken = take(session, proof, nowMs, nonce, sig);
    if (taken != Result::Ok) return taken;
    return PairingCrypto::verify(spki, message(hostId, nonce, action, slot), sig) ? Result::Ok
                                                                                  : Result::Refused;
}

OwnerProof::Result OwnerProof::checkFields(const QString& session, const QByteArray& spki,
                                           const QString& hostId, const QList<QByteArray>& fields,
                                           const QJsonObject& proof, qint64 nowMs,
                                           const QString& scope, bool* badSignature)
{
    if (badSignature) *badSignature = false;
    QByteArray nonce;
    QByteArray sig;
    const Result taken = take(session, proof, nowMs, nonce, sig, scope);
    if (taken != Result::Ok) return taken;
    if (PairingCrypto::verify(spki, messageFor(hostId, nonce, fields), sig)) return Result::Ok;
    if (badSignature) *badSignature = true;
    return Result::Refused;
}

void OwnerProof::dropScope(const QString& scope)
{
    if (scope.isEmpty()) return;
    for (int i = m_Pending.size() - 1; i >= 0; --i)
        if (m_Pending.at(i).scope == scope) m_Pending.removeAt(i);
}

bool OwnerProof::isBoothAction(const QString& action)
{
    return action == QLatin1String("booth_join") || action == QLatin1String("booth_takeover") ||
           action == QLatin1String("booth_pin_read") || action == QLatin1String("booth_pin_new");
}

QByteArray OwnerProof::boothMessage(const QString& hostId, const QByteArray& nonce,
                                    const QString& action, const QString& hostUuid)
{
    return PairingCrypto::buildSignedMessage(
        {QByteArray(kBoothLabel), hostId.toUtf8(), nonce, action.toUtf8(), hostUuid.toUtf8()});
}

OwnerProof::Result OwnerProof::checkBooth(const QString& session, const QByteArray& spki,
                                          const QString& hostId, const QString& action,
                                          const QString& hostUuid, const QJsonObject& proof,
                                          qint64 nowMs)
{
    // Spent first, whatever comes next: a challenge never verifies twice.
    QByteArray nonce;
    QByteArray sig;
    const Result taken = take(session, proof, nowMs, nonce, sig);
    if (spki.isEmpty()) return Result::NoKey;
    if (taken != Result::Ok) return taken;
    if (!isBoothAction(action) || hostUuid.isEmpty()) return Result::Refused;
    return PairingCrypto::verify(spki, boothMessage(hostId, nonce, action, hostUuid), sig)
               ? Result::Ok
               : Result::Refused;
}

void OwnerProof::prune(qint64 nowMs)
{
    for (int i = m_Pending.size() - 1; i >= 0; --i)
        if (m_Pending.at(i).expiresMs <= nowMs) m_Pending.removeAt(i);
}
